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

#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_discovery_manager.h"

#include <gmock/gmock.h>
#include <pw_assert/check.h>

#include <unordered_set>
#include <vector>

#include "pw_bluetooth_sapphire/internal/host/common/advertising_data.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/gap/peer.h"
#include "pw_bluetooth_sapphire/internal/host/gap/peer_cache.h"
#include "pw_bluetooth_sapphire/internal/host/hci/advertising_packet_filter.h"
#include "pw_bluetooth_sapphire/internal/host/hci/discovery_filter.h"
#include "pw_bluetooth_sapphire/internal/host/hci/extended_low_energy_scanner.h"
#include "pw_bluetooth_sapphire/internal/host/hci/fake_local_address_delegate.h"
#include "pw_bluetooth_sapphire/internal/host/hci/legacy_low_energy_scanner.h"
#include "pw_bluetooth_sapphire/internal/host/testing/controller_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_controller.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_peer.h"
#include "pw_bluetooth_sapphire/internal/host/testing/inspect.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"

namespace bt::gap {
namespace {

using namespace inspect::testing;
using bt::testing::FakeController;
using bt::testing::FakePeer;
using PauseToken = LowEnergyDiscoveryManager::PauseToken;

using TestingBase = bt::testing::FakeDispatcherControllerTest<FakeController>;

const DeviceAddress kAddress0(DeviceAddress::Type::kLEPublic, {0});
const DeviceAddress kAddrAlias0(DeviceAddress::Type::kBREDR, kAddress0.value());
const DeviceAddress kAddress1(DeviceAddress::Type::kLERandom, {1});
const DeviceAddress kAddress2(DeviceAddress::Type::kLEPublic, {2});
const DeviceAddress kAddress3(DeviceAddress::Type::kLEPublic, {3});
const DeviceAddress kAddress4(DeviceAddress::Type::kLEPublic, {4});
const DeviceAddress kAddress5(DeviceAddress::Type::kLEPublic, {5});

constexpr uint16_t kServiceDataUuid = 0x1234;

constexpr pw::chrono::SystemClock::duration kTestScanPeriod =
    std::chrono::seconds(10);

const char* kInspectNodeName = "low_energy_discovery_manager";

class LowEnergyDiscoveryManagerTest : public TestingBase {
 public:
  LowEnergyDiscoveryManagerTest() = default;
  ~LowEnergyDiscoveryManagerTest() override = default;

  void SetUp() override {
    TestingBase::SetUp();

    scan_enabled_ = false;

    FakeController::Settings settings;
    settings.ApplyExtendedLEConfig();
    test_device()->set_settings(settings);

    test_device()->set_scan_state_callback([this](auto&& PH1) {
      OnScanStateChanged(std::forward<decltype(PH1)>(PH1));
    });

    SetupDiscoveryManager();
  }

  void TearDown() override {
    discovery_manager_ = nullptr;
    scanner_ = nullptr;
    test_device()->Stop();
    TestingBase::TearDown();
  }

 protected:
  void SetupDiscoveryManager(
      bool extended = false,
      hci::AdvertisingPacketFilter::Config packet_filter_config = {
          false,
          0,
          hci::AdvertisingPacketFilter::Config::DeliveryMode::kImmediate}) {
    discovery_manager_ = nullptr;
    if (extended) {
      scanner_ = std::make_unique<hci::ExtendedLowEnergyScanner>(
          &fake_address_delegate_,
          packet_filter_config,
          transport()->GetWeakPtr(),
          dispatcher());
    } else {
      scanner_ = std::make_unique<hci::LegacyLowEnergyScanner>(
          &fake_address_delegate_,
          packet_filter_config,
          transport()->GetWeakPtr(),
          dispatcher());
    }

    discovery_manager_ = std::make_unique<LowEnergyDiscoveryManager>(
        scanner_.get(), &peer_cache_, packet_filter_config, dispatcher());
    discovery_manager_->AttachInspect(inspector_.GetRoot(), kInspectNodeName);
  }

  LowEnergyDiscoveryManager* discovery_manager() const {
    return discovery_manager_.get();
  }

  // Deletes |discovery_manager_|.
  void DeleteDiscoveryManager() { discovery_manager_ = nullptr; }

#ifndef NINSPECT
  inspect::Hierarchy InspectHierarchy() const {
    return inspect::ReadFromVmo(inspector_.DuplicateVmo()).take_value();
  }

  std::vector<inspect::PropertyValue> InspectProperties() const {
    auto hierarchy = InspectHierarchy();
    auto children = hierarchy.take_children();
    PW_CHECK(children.size() == 1u);
    return children.front().node_ptr()->take_properties();
  }
#endif  // NINSPECT

  PeerCache* peer_cache() { return &peer_cache_; }

  // Returns the last reported scan state of the FakeController.
  bool scan_enabled() const { return scan_enabled_; }

  // The scan states that the FakeController has transitioned through.
  const std::vector<bool> scan_states() const { return scan_states_; }

  // Sets a callback that will run when the scan state transitions |count|
  // times.
  void set_scan_state_handler(size_t count, fit::closure callback) {
    scan_state_callbacks_[count] = std::move(callback);
  }

  // Called by FakeController when the scan state changes.
  void OnScanStateChanged(bool enabled) {
    auto scan_type = test_device()->le_scan_state().scan_type;
    bt_log(DEBUG,
           "gap-test",
           "FakeController scan state: %s %s",
           enabled ? "enabled" : "disabled",
           scan_type == pw::bluetooth::emboss::LEScanType::ACTIVE ? "active"
                                                                  : "passive");
    scan_enabled_ = enabled;
    scan_states_.push_back(enabled);

    auto iter = scan_state_callbacks_.find(scan_states_.size());
    if (iter != scan_state_callbacks_.end()) {
      iter->second();
    }
  }

  // Registers the following fake peers with the FakeController:
  //
  // Peer 0:
  //   - Connectable, not scannable;
  //   - General discoverable;
  //   - UUIDs: 0x180d, 0x180f;
  //   - Service Data UUIDs: kServiceDataUuid;
  //   - has name: "Device 0"
  //
  // Peer 1:
  //   - Connectable, not scannable;
  //   - Limited discoverable;
  //   - UUIDs: 0x180d;
  //   - has name: "Device 1"
  //
  // Peer 2:
  //   - Not connectable, not scannable;
  //   - General discoverable;
  //   - UUIDs: none;
  //   - has name: "Device 2"
  //
  // Peer 3:
  //   - Not discoverable;
  void AddFakePeers() {
    // Peer 0
    const StaticByteBuffer kAdvData0(
        // Flags
        0x02,
        0x01,
        0x02,

        // Complete 16-bit service UUIDs
        0x05,
        0x03,
        0x0d,
        0x18,
        0x0f,
        0x18,

        // 16-bit service data UUID
        0x03,
        DataType::kServiceData16Bit,
        LowerBits(kServiceDataUuid),
        UpperBits(kServiceDataUuid),

        // Complete local name
        0x09,
        0x09,
        'D',
        'e',
        'v',
        'i',
        'c',
        'e',
        ' ',
        '0');
    auto fake_peer =
        std::make_unique<FakePeer>(kAddress0, dispatcher(), true, true);
    fake_peer->set_advertising_data(kAdvData0);
    test_device()->AddPeer(std::move(fake_peer));

    // Peer 1
    const StaticByteBuffer kAdvData1(
        // Flags
        0x02,
        0x01,
        0x01,

        // Complete 16-bit service UUIDs
        0x03,
        0x03,
        0x0d,
        0x18);
    fake_peer = std::make_unique<FakePeer>(kAddress1, dispatcher(), true, true);
    fake_peer->set_advertising_data(kAdvData1);
    test_device()->AddPeer(std::move(fake_peer));

    // Peer 2
    const StaticByteBuffer kAdvData2(
        // Flags
        0x02,
        0x01,
        0x02,

        // Complete local name
        0x09,
        0x09,
        'D',
        'e',
        'v',
        'i',
        'c',
        'e',
        ' ',
        '2');
    fake_peer =
        std::make_unique<FakePeer>(kAddress2, dispatcher(), false, false);
    fake_peer->set_advertising_data(kAdvData2);
    test_device()->AddPeer(std::move(fake_peer));

    // Peer 3
    const StaticByteBuffer kAdvData3(
        // Flags
        0x02,
        0x01,
        0x00,

        // Complete local name
        0x09,
        0x09,
        'D',
        'e',
        'v',
        'i',
        'c',
        'e',
        ' ',
        '3');
    fake_peer =
        std::make_unique<FakePeer>(kAddress3, dispatcher(), false, false);
    fake_peer->set_advertising_data(kAdvData3);
    test_device()->AddPeer(std::move(fake_peer));
  }

  // Creates and returns a discovery session.
  std::unique_ptr<LowEnergyDiscoverySession> StartDiscoverySession(
      bool active = true,
      std::vector<hci::DiscoveryFilter> discovery_filters = {}) {
    std::unique_ptr<LowEnergyDiscoverySession> session;
    discovery_manager()->StartDiscovery(
        active, std::move(discovery_filters), [&](auto cb_session) {
          PW_CHECK(cb_session);
          session = std::move(cb_session);
        });

    RunUntilIdle();
    PW_CHECK(session);
    return session;
  }

 private:
  PeerCache peer_cache_{dispatcher()};
  hci::FakeLocalAddressDelegate fake_address_delegate_{dispatcher()};
  std::unique_ptr<hci::LowEnergyScanner> scanner_;
  std::unique_ptr<LowEnergyDiscoveryManager> discovery_manager_;

  bool scan_enabled_;
  std::vector<bool> scan_states_;
  std::unordered_map<size_t, fit::closure> scan_state_callbacks_;

  inspect::Inspector inspector_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(LowEnergyDiscoveryManagerTest);
};

using GAP_LowEnergyDiscoveryManagerTest = LowEnergyDiscoveryManagerTest;

TEST_F(LowEnergyDiscoveryManagerTest, StartDiscoveryAndStop) {
  std::unique_ptr<LowEnergyDiscoverySession> session;
  discovery_manager()->StartDiscovery(
      /*active=*/true, {}, [&session](auto cb_session) {
        session = std::move(cb_session);
      });

  RunUntilIdle();

  // The test fixture will be notified of the change in scan state before we
  // receive the session.
  EXPECT_TRUE(scan_enabled());
  RunUntilIdle();

  ASSERT_TRUE(session);
  EXPECT_TRUE(session->alive());

  session->Stop();

  RunUntilIdle();
  EXPECT_FALSE(scan_enabled());
}

TEST_F(LowEnergyDiscoveryManagerTest, StartDiscoveryAndStopByDeleting) {
  // Start discovery but don't acquire ownership of the received session. This
  // should immediately terminate the session.
  std::unique_ptr<LowEnergyDiscoverySession> session;
  discovery_manager()->StartDiscovery(
      /*active=*/true, {}, [&session](auto cb_session) {
        session = std::move(cb_session);
      });

  RunUntilIdle();

  // The test fixture will be notified of the change in scan state before we
  // receive the session.
  EXPECT_TRUE(scan_enabled());
  RunUntilIdle();

  ASSERT_TRUE(session);
  EXPECT_TRUE(session->alive());

  session = nullptr;

  RunUntilIdle();
  EXPECT_FALSE(scan_enabled());
}

TEST_F(LowEnergyDiscoveryManagerTest, Destructor) {
  // Start discovery with a session, delete the manager and ensure that the
  // session is inactive with the error callback called.
  std::unique_ptr<LowEnergyDiscoverySession> session;
  discovery_manager()->StartDiscovery(
      /*active=*/true, {}, [&session](auto cb_session) {
        session = std::move(cb_session);
      });

  RunUntilIdle();

  EXPECT_TRUE(scan_enabled());

  ASSERT_TRUE(session);
  EXPECT_TRUE(session->alive());

  size_t num_errors = 0u;
  session->set_error_callback([&num_errors]() { num_errors++; });

  EXPECT_EQ(0u, num_errors);
  DeleteDiscoveryManager();
  EXPECT_EQ(1u, num_errors);
  EXPECT_FALSE(session->alive());
}

TEST_F(LowEnergyDiscoveryManagerTest, StartDiscoveryAndStopInCallback) {
  // Start discovery but don't acquire ownership of the received session. This
  // should terminate the session when |session| goes out of scope.
  discovery_manager()->StartDiscovery(/*active=*/true, {}, [](auto) {});

  RunUntilIdle();
  ASSERT_EQ(2u, scan_states().size());
  EXPECT_TRUE(scan_states()[0]);
  EXPECT_FALSE(scan_states()[1]);
}

TEST_F(LowEnergyDiscoveryManagerTest, StartDiscoveryFailure) {
  test_device()->SetDefaultResponseStatus(
      hci_spec::kLESetScanEnable,
      pw::bluetooth::emboss::StatusCode::COMMAND_DISALLOWED);

  // |session| should contain nullptr.
  discovery_manager()->StartDiscovery(
      /*active=*/true, {}, [](auto session) { EXPECT_FALSE(session); });

  RunUntilIdle();
  EXPECT_FALSE(scan_enabled());
}

TEST_F(LowEnergyDiscoveryManagerTest, StartDiscoveryWhileScanning) {
  std::vector<std::unique_ptr<LowEnergyDiscoverySession>> sessions;

  constexpr size_t kExpectedSessionCount = 5;
  size_t cb_count = 0u;
  auto cb = [&cb_count, &sessions](auto session) {
    sessions.push_back(std::move(session));
    cb_count++;
  };

  discovery_manager()->StartDiscovery(/*active=*/true, {}, cb);

  RunUntilIdle();
  EXPECT_TRUE(scan_enabled());
  EXPECT_EQ(1u, sessions.size());

  // Add the rest of the sessions. These are expected to succeed immediately but
  // the callbacks should be called asynchronously.
  for (size_t i = 1u; i < kExpectedSessionCount; i++) {
    discovery_manager()->StartDiscovery(/*active=*/true, {}, cb);
  }

  RunUntilIdle();
  EXPECT_TRUE(scan_enabled());
  EXPECT_EQ(kExpectedSessionCount, sessions.size());

  // Remove one session from the list. Scan should continue.
  sessions.pop_back();
  RunUntilIdle();
  EXPECT_TRUE(scan_enabled());

  // Remove all but one session from the list. Scan should continue.
  sessions.erase(sessions.begin() + 1, sessions.end());
  RunUntilIdle();
  EXPECT_TRUE(scan_enabled());
  EXPECT_EQ(1u, sessions.size());

  // Remove the last session.
  sessions.clear();
  RunUntilIdle();
  EXPECT_FALSE(scan_enabled());
}

TEST_F(LowEnergyDiscoveryManagerTest, StartDiscoveryWhilePendingStart) {
  std::vector<std::unique_ptr<LowEnergyDiscoverySession>> sessions;

  constexpr size_t kExpectedSessionCount = 5;
  size_t cb_count = 0u;
  auto cb = [&cb_count, &sessions](auto session) {
    sessions.push_back(std::move(session));
    cb_count++;
  };

  for (size_t i = 0u; i < kExpectedSessionCount; i++) {
    discovery_manager()->StartDiscovery(/*active=*/true, {}, cb);
  }

  RunUntilIdle();
  EXPECT_TRUE(scan_enabled());
  EXPECT_EQ(kExpectedSessionCount, sessions.size());

  // Remove all sessions. This should stop the scan.
  sessions.clear();
  RunUntilIdle();
  EXPECT_FALSE(scan_enabled());
}

TEST_F(LowEnergyDiscoveryManagerTest,
       StartDiscoveryWhilePendingStartAndStopInCallback) {
  constexpr size_t kExpectedSessionCount = 5;
  size_t cb_count = 0u;
  std::unique_ptr<LowEnergyDiscoverySession> session;
  auto cb = [&cb_count, &session](auto cb_session) {
    cb_count++;
    if (cb_count == kExpectedSessionCount) {
      // Hold on to only the last session object. The rest should get deleted
      // within the callback.
      session = std::move(cb_session);
    }
  };

  for (size_t i = 0u; i < kExpectedSessionCount; i++) {
    discovery_manager()->StartDiscovery(/*active=*/true, {}, cb);
  }

  RunUntilIdle();
  EXPECT_TRUE(scan_enabled());
  EXPECT_TRUE(session);

  RunUntilIdle();
  EXPECT_EQ(kExpectedSessionCount, cb_count);
  EXPECT_TRUE(scan_enabled());

  // Deleting the only remaning session should stop the scan.
  session = nullptr;
  RunUntilIdle();
  EXPECT_FALSE(scan_enabled());
}

TEST_F(LowEnergyDiscoveryManagerTest, StartDiscoveryWhilePendingStop) {
  std::unique_ptr<LowEnergyDiscoverySession> session;

  discovery_manager()->StartDiscovery(
      /*active=*/true, {}, [&session](auto cb_session) {
        session = std::move(cb_session);
      });

  RunUntilIdle();
  EXPECT_TRUE(scan_enabled());
  EXPECT_TRUE(session);

  // Stop the session. This should issue a request to stop the ongoing scan but
  // the request will remain pending until we run the message loop.
  session = nullptr;

  // Request a new session. The discovery manager should restart the scan after
  // the ongoing one stops.
  discovery_manager()->StartDiscovery(
      /*active=*/true, {}, [&session](auto cb_session) {
        session = std::move(cb_session);
      });

  // Discovery should stop and start again.
  RunUntilIdle();
  ASSERT_EQ(3u, scan_states().size());
  EXPECT_TRUE(scan_states()[0]);
  EXPECT_FALSE(scan_states()[1]);
  EXPECT_TRUE(scan_states()[2]);
}

TEST_F(LowEnergyDiscoveryManagerTest, StartDiscoveryFailureManyPending) {
  test_device()->SetDefaultResponseStatus(
      hci_spec::kLESetScanEnable,
      pw::bluetooth::emboss::StatusCode::COMMAND_DISALLOWED);

  constexpr size_t kExpectedSessionCount = 5;
  size_t cb_count = 0u;
  auto cb = [&cb_count](auto session) {
    // |session| should contain nullptr as the request will fail.
    EXPECT_FALSE(session);
    cb_count++;
  };

  for (size_t i = 0u; i < kExpectedSessionCount; i++) {
    discovery_manager()->StartDiscovery(/*active=*/true, {}, cb);
  }

  RunUntilIdle();
  EXPECT_FALSE(scan_enabled());
}

TEST_F(LowEnergyDiscoveryManagerTest, ScanPeriodRestart) {
  constexpr size_t kNumScanStates = 3;

  discovery_manager()->set_scan_period(kTestScanPeriod);

  std::unique_ptr<LowEnergyDiscoverySession> session;
  discovery_manager()->StartDiscovery(
      /*active=*/true, {}, [&session](auto cb_session) {
        session = std::move(cb_session);
      });

  // We should observe the scan state become enabled -> disabled -> enabled.
  RunUntilIdle();
  EXPECT_TRUE(scan_enabled());

  // End the scan period.
  RunFor(kTestScanPeriod);
  ASSERT_EQ(kNumScanStates, scan_states().size());
  EXPECT_TRUE(scan_states()[0]);
  EXPECT_FALSE(scan_states()[1]);
  EXPECT_TRUE(scan_states()[2]);
}

TEST_F(LowEnergyDiscoveryManagerTest, ScanPeriodRestartFailure) {
  constexpr size_t kNumScanStates = 2;

  discovery_manager()->set_scan_period(kTestScanPeriod);

  std::unique_ptr<LowEnergyDiscoverySession> session;
  bool session_error = false;
  discovery_manager()->StartDiscovery(
      /*active=*/true, {}, [&](auto cb_session) {
        session = std::move(cb_session);
        session->set_error_callback([&session_error] { session_error = true; });
      });

  // The controller will fail to restart scanning after scanning stops at the
  // end of the period. The scan state will transition twice (-> enabled ->
  // disabled).
  set_scan_state_handler(kNumScanStates, [this] {
    test_device()->SetDefaultResponseStatus(
        hci_spec::kLESetScanEnable,
        pw::bluetooth::emboss::StatusCode::COMMAND_DISALLOWED);
  });

  RunUntilIdle();
  EXPECT_TRUE(scan_enabled());

  // End the scan period. The scan should not restart.
  RunFor(kTestScanPeriod);

  ASSERT_EQ(kNumScanStates, scan_states().size());
  EXPECT_TRUE(scan_states()[0]);
  EXPECT_FALSE(scan_states()[1]);
  EXPECT_TRUE(session_error);
}

TEST_F(LowEnergyDiscoveryManagerTest, ScanPeriodRestartRemoveSession) {
  constexpr size_t kNumScanStates = 4;

  discovery_manager()->set_scan_period(kTestScanPeriod);

  std::unique_ptr<LowEnergyDiscoverySession> session;
  discovery_manager()->StartDiscovery(
      /*active=*/true, {}, [&session](auto cb_session) {
        session = std::move(cb_session);
      });

  // We should observe 3 scan state transitions (-> enabled -> disabled ->
  // enabled).
  set_scan_state_handler(kNumScanStates - 1, [this, &session] {
    ASSERT_TRUE(session);
    EXPECT_TRUE(scan_enabled());

    // At this point the fake controller has updated its state but the discovery
    // manager has not processed the restarted scan. We should be able to remove
    // the current session and the state should ultimately become disabled.
    session.reset();
  });

  RunUntilIdle();
  EXPECT_TRUE(scan_enabled());

  // End the scan period.
  RunFor(kTestScanPeriod);
  EXPECT_THAT(scan_states(), ::testing::ElementsAre(true, false, true, false));
}

TEST_F(LowEnergyDiscoveryManagerTest, ScanPeriodRemoveSessionDuringRestart) {
  constexpr size_t kNumScanStates = 2;

  // Set a very short scan period for the sake of the test.
  discovery_manager()->set_scan_period(kTestScanPeriod);

  std::unique_ptr<LowEnergyDiscoverySession> session;
  discovery_manager()->StartDiscovery(
      /*active=*/true, {}, [&session](auto cb_session) {
        session = std::move(cb_session);
      });

  // The controller will fail to restart scanning after scanning stops at the
  // end of the period. The scan state will transition twice (-> enabled ->
  // disabled).
  set_scan_state_handler(kNumScanStates, [this, &session] {
    ASSERT_TRUE(session);
    EXPECT_FALSE(scan_enabled());

    // Stop the session before the discovery manager processes the event. It
    // should detect this and discontinue the scan.
    session.reset();
  });

  RunUntilIdle();
  EXPECT_TRUE(scan_enabled());

  // End the scan period.
  RunFor(kTestScanPeriod);
  EXPECT_THAT(scan_states(), ::testing::ElementsAre(true, false));
}

TEST_F(LowEnergyDiscoveryManagerTest, ScanPeriodRestartRemoveAndAddSession) {
  constexpr size_t kNumScanPeriodRestartStates = 3;
  constexpr size_t kTotalNumStates = 5;

  // Set a very short scan period for the sake of the test.
  discovery_manager()->set_scan_period(kTestScanPeriod);

  std::unique_ptr<LowEnergyDiscoverySession> session;
  auto cb = [&session](auto cb_session) { session = std::move(cb_session); };
  discovery_manager()->StartDiscovery(/*active=*/true, {}, cb);

  // We should observe 3 scan state transitions (-> enabled -> disabled ->
  // enabled).
  set_scan_state_handler(kNumScanPeriodRestartStates, [this, &session, cb] {
    ASSERT_TRUE(session);
    EXPECT_TRUE(scan_enabled());

    // At this point the fake controller has updated its state but the discovery
    // manager has not processed the restarted scan. We should be able to remove
    // the current session and create a new one and the state should update
    // accordingly.
    session.reset();
    discovery_manager()->StartDiscovery(/*active=*/true, {}, cb);
  });

  RunUntilIdle();
  EXPECT_TRUE(scan_enabled());

  // End the scan period.
  RunFor(kTestScanPeriod);

  // Scan should have been disabled and re-enabled.
  ASSERT_EQ(kTotalNumStates, scan_states().size());
  EXPECT_TRUE(scan_states()[0]);
  EXPECT_FALSE(scan_states()[1]);
  EXPECT_TRUE(scan_states()[2]);
}

TEST_F(LowEnergyDiscoveryManagerTest, StartDiscoveryWithFilters) {
  AddFakePeers();

  std::vector<std::unique_ptr<LowEnergyDiscoverySession>> sessions;

  // Set a short scan period so that we that we process events for multiple scan
  // periods during the test.
  discovery_manager()->set_scan_period(std::chrono::milliseconds(200));

  // Session 0 is interested in performing general discovery.
  std::unordered_set<DeviceAddress> peers_session0;
  LowEnergyDiscoverySession::PeerFoundFunction result_cb =
      [&peers_session0](const auto& peer) {
        peers_session0.insert(peer.address());
      };

  hci::DiscoveryFilter discovery_filter;
  discovery_filter.SetGeneralDiscoveryFlags();
  std::vector<hci::DiscoveryFilter> discovery_filters;
  discovery_filters.push_back(discovery_filter);
  sessions.push_back(StartDiscoverySession(/*active=*/true, discovery_filters));
  sessions[0]->SetResultCallback(std::move(result_cb));

  // Session 1 is interested in performing limited discovery.
  hci::DiscoveryFilter discovery_filter1;
  discovery_filter1.set_flags(AdvFlag::kLELimitedDiscoverableMode);
  std::vector<hci::DiscoveryFilter> discovery_filters1;
  discovery_filters1.push_back(discovery_filter1);

  std::unordered_set<DeviceAddress> peers_session1;
  result_cb = [&peers_session1](const auto& peer) {
    peers_session1.insert(peer.address());
  };
  sessions.push_back(
      StartDiscoverySession(/*active=*/true, discovery_filters1));
  sessions[1]->SetResultCallback(std::move(result_cb));

  // Session 2 is interested in peers with UUID 0x180d.
  hci::DiscoveryFilter discovery_filter2;
  uint16_t uuid = 0x180d;
  discovery_filter2.set_service_uuids({UUID(uuid)});
  std::vector<hci::DiscoveryFilter> discovery_filters2;
  discovery_filters2.push_back(discovery_filter2);

  std::unordered_set<DeviceAddress> peers_session2;
  result_cb = [&peers_session2](const auto& peer) {
    peers_session2.insert(peer.address());
  };
  sessions.push_back(
      StartDiscoverySession(/*active=*/true, discovery_filters2));
  sessions[2]->SetResultCallback(std::move(result_cb));

  // Session 3 is interested in peers whose names contain "Device".
  hci::DiscoveryFilter discovery_filter3;
  discovery_filter3.set_name_substring("Device");
  std::vector<hci::DiscoveryFilter> discovery_filters3;
  discovery_filters3.push_back(discovery_filter3);

  std::unordered_set<DeviceAddress> peers_session3;
  result_cb = [&peers_session3](const auto& peer) {
    peers_session3.insert(peer.address());
  };
  sessions.push_back(
      StartDiscoverySession(/*active=*/true, discovery_filters3));
  sessions[3]->SetResultCallback(std::move(result_cb));

  // Session 4 is interested in non-connectable peers.
  hci::DiscoveryFilter discovery_filter4;
  discovery_filter4.set_connectable(false);
  std::vector<hci::DiscoveryFilter> discovery_filters4;
  discovery_filters4.push_back(discovery_filter4);

  std::unordered_set<DeviceAddress> peers_session4;
  result_cb = [&peers_session4](const auto& peer) {
    peers_session4.insert(peer.address());
  };
  sessions.push_back(
      StartDiscoverySession(/*active=*/true, discovery_filters4));
  sessions[4]->SetResultCallback(std::move(result_cb));

  // Session 5 is interested in peers with UUID 0x180d and service data UUID
  // 0x1234.
  hci::DiscoveryFilter discovery_filter5;
  discovery_filter5.set_service_uuids({UUID(uuid)});
  discovery_filter5.set_service_data_uuids({UUID(kServiceDataUuid)});
  std::vector<hci::DiscoveryFilter> discovery_filters5;
  discovery_filters5.push_back(discovery_filter5);

  std::unordered_set<DeviceAddress> peers_session5;
  result_cb = [&peers_session5](const auto& peer) {
    peers_session5.insert(peer.address());
  };
  sessions.push_back(
      StartDiscoverySession(/*active=*/true, discovery_filters5));
  sessions[5]->SetResultCallback(std::move(result_cb));

  RunUntilIdle();
  EXPECT_EQ(6u, sessions.size());

  // At this point all sessions should have processed all peers at least once.

  // Session 0: Should have seen all peers except for peer 3, which is
  // non-discoverable.
  EXPECT_EQ(3u, peers_session0.size());
  EXPECT_THAT(peers_session0, ::testing::Contains(kAddress0));
  EXPECT_THAT(peers_session0, ::testing::Contains(kAddress1));
  EXPECT_THAT(peers_session0, ::testing::Contains(kAddress2));

  // Session 1: Should have only seen peer 1.
  EXPECT_EQ(1u, peers_session1.size());
  EXPECT_THAT(peers_session1, ::testing::Contains(kAddress1));

  // Session 2: Should have only seen peers 0 and 1
  EXPECT_EQ(2u, peers_session2.size());
  EXPECT_THAT(peers_session2, ::testing::Contains(kAddress0));
  EXPECT_THAT(peers_session2, ::testing::Contains(kAddress1));

  // Session 3: Should have only seen peers 0, 2, and 3
  EXPECT_EQ(3u, peers_session3.size());
  EXPECT_THAT(peers_session3, ::testing::Contains(kAddress0));
  EXPECT_THAT(peers_session3, ::testing::Contains(kAddress2));
  EXPECT_THAT(peers_session3, ::testing::Contains(kAddress3));

  // Session 4: Should have seen peers 2 and 3
  EXPECT_EQ(2u, peers_session4.size());
  EXPECT_THAT(peers_session4, ::testing::Contains(kAddress2));
  EXPECT_THAT(peers_session4, ::testing::Contains(kAddress3));

  // Session 5: Should only see peer 0.
  EXPECT_EQ(1u, peers_session5.size());
  EXPECT_THAT(peers_session5, ::testing::Contains(kAddress0));
}

TEST_F(LowEnergyDiscoveryManagerTest,
       StartDiscoveryWithFiltersCachedPeerNotifications) {
  AddFakePeers();

  std::vector<std::unique_ptr<LowEnergyDiscoverySession>> sessions;

  // Set a long scan period to make sure that the FakeController sends
  // advertising reports only once.
  discovery_manager()->set_scan_period(std::chrono::seconds(20));

  // Session 0 is interested in performing general discovery.
  hci::DiscoveryFilter discovery_filter;
  discovery_filter.SetGeneralDiscoveryFlags();
  std::vector<hci::DiscoveryFilter> discovery_filters;
  discovery_filters.push_back(discovery_filter);

  std::unordered_set<DeviceAddress> peers_session0;
  LowEnergyDiscoverySession::PeerFoundFunction result_cb =
      [&peers_session0](const auto& peer) {
        peers_session0.insert(peer.address());
      };
  sessions.push_back(StartDiscoverySession(/*active=*/true, discovery_filters));
  sessions[0]->SetResultCallback(std::move(result_cb));

  RunUntilIdle();
  ASSERT_EQ(3u, peers_session0.size());

  // Session 1 is interested in performing limited discovery.
  hci::DiscoveryFilter discovery_filter1;
  discovery_filter1.set_flags(AdvFlag::kLELimitedDiscoverableMode);
  std::vector<hci::DiscoveryFilter> discovery_filters1;
  discovery_filters1.push_back(discovery_filter1);

  std::unordered_set<DeviceAddress> peers_session1;
  result_cb = [&peers_session1](const auto& peer) {
    peers_session1.insert(peer.address());
  };
  sessions.push_back(
      StartDiscoverySession(/*active=*/true, discovery_filters1));
  sessions[1]->SetResultCallback(std::move(result_cb));

  // Session 2 is interested in peers with UUID 0x180d.
  hci::DiscoveryFilter discovery_filter2;
  uint16_t uuid = 0x180d;
  discovery_filter2.set_service_uuids({UUID(uuid)});
  std::vector<hci::DiscoveryFilter> discovery_filters2;
  discovery_filters2.push_back(discovery_filter2);

  std::unordered_set<DeviceAddress> peers_session2;
  result_cb = [&peers_session2](const auto& peer) {
    peers_session2.insert(peer.address());
  };
  sessions.push_back(
      StartDiscoverySession(/*active=*/true, discovery_filters2));

  sessions[2]->SetResultCallback(std::move(result_cb));

  // Session 3 is interested in peers whose names contain "Device".
  hci::DiscoveryFilter discovery_filter3;
  discovery_filter3.set_name_substring("Device");
  std::vector<hci::DiscoveryFilter> discovery_filters3;
  discovery_filters3.push_back(discovery_filter3);

  std::unordered_set<DeviceAddress> peers_session3;
  result_cb = [&peers_session3](const auto& peer) {
    peers_session3.insert(peer.address());
  };
  sessions.push_back(
      StartDiscoverySession(/*active=*/true, discovery_filters3));
  sessions[3]->SetResultCallback(std::move(result_cb));

  // Session 4 is interested in non-connectable peers.
  hci::DiscoveryFilter discovery_filter4;
  discovery_filter4.set_connectable(false);
  std::vector<hci::DiscoveryFilter> discovery_filters4;
  discovery_filters4.push_back(discovery_filter4);

  std::unordered_set<DeviceAddress> peers_session4;
  result_cb = [&peers_session4](const auto& peer) {
    peers_session4.insert(peer.address());
  };
  sessions.push_back(
      StartDiscoverySession(/*active=*/true, discovery_filters4));
  sessions[4]->SetResultCallback(std::move(result_cb));

  EXPECT_EQ(5u, sessions.size());
  RunUntilIdle();

#define EXPECT_CONTAINS(addr, dev_list) \
  EXPECT_TRUE(dev_list.find(addr) != dev_list.end())
  // At this point all sessions should have processed all peers at least once
  // without running the message loop; results for Sessions 1, 2, 3, and 4
  // should have come from the cache.

  // Session 0: Should have seen all peers except for peer 3, which is
  // non-discoverable.
  EXPECT_EQ(3u, peers_session0.size());
  EXPECT_CONTAINS(kAddress0, peers_session0);
  EXPECT_CONTAINS(kAddress1, peers_session0);
  EXPECT_CONTAINS(kAddress2, peers_session0);

  // Session 1: Should have only seen peer 1.
  EXPECT_EQ(1u, peers_session1.size());
  EXPECT_CONTAINS(kAddress1, peers_session1);

  // Session 2: Should have only seen peers 0 and 1
  EXPECT_EQ(2u, peers_session2.size());
  EXPECT_CONTAINS(kAddress0, peers_session2);
  EXPECT_CONTAINS(kAddress1, peers_session2);

  // Session 3: Should have only seen peers 0, 2, and 3
  EXPECT_EQ(3u, peers_session3.size());
  EXPECT_CONTAINS(kAddress0, peers_session3);
  EXPECT_CONTAINS(kAddress2, peers_session3);
  EXPECT_CONTAINS(kAddress3, peers_session3);

  // Session 4: Should have seen peers 2 and 3
  EXPECT_EQ(2u, peers_session4.size());
  EXPECT_CONTAINS(kAddress2, peers_session4);
  EXPECT_CONTAINS(kAddress3, peers_session4);

#undef EXPECT_CONTAINS
}

TEST_F(LowEnergyDiscoveryManagerTest, DirectedAdvertisingEventFromUnknownPeer) {
  auto fake_peer = std::make_unique<FakePeer>(kAddress0,
                                              dispatcher(),
                                              /*connectable=*/true,
                                              /*scannable=*/false);
  fake_peer->set_directed_advertising_enabled(true);
  test_device()->AddPeer(std::move(fake_peer));

  int connectable_count = 0;
  discovery_manager()->set_peer_connectable_callback(
      [&](auto) { connectable_count++; });
  discovery_manager()->set_scan_period(kTestScanPeriod);

  auto active_session = StartDiscoverySession();
  int active_count = 0;
  active_session->SetResultCallback([&](auto&) { active_count++; });

  auto passive_session = StartDiscoverySession(/*active=*/false);
  int passive_count = 0;
  passive_session->SetResultCallback([&](auto&) { passive_count++; });

  RunUntilIdle();
  ASSERT_TRUE(active_session);
  ASSERT_TRUE(passive_session);
  EXPECT_EQ(0, connectable_count);
  EXPECT_EQ(0, active_count);
  EXPECT_EQ(0, passive_count);
}

TEST_F(LowEnergyDiscoveryManagerTest,
       DirectedAdvertisingEventFromKnownNonConnectablePeer) {
  auto fake_peer = std::make_unique<FakePeer>(kAddress0,
                                              dispatcher(),
                                              /*connectable=*/false,
                                              /*scannable=*/false);
  fake_peer->set_directed_advertising_enabled(true);
  test_device()->AddPeer(std::move(fake_peer));
  Peer* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/false);
  ASSERT_TRUE(peer);

  int connectable_count = 0;
  discovery_manager()->set_peer_connectable_callback(
      [&](auto) { connectable_count++; });
  discovery_manager()->set_scan_period(kTestScanPeriod);

  auto active_session = StartDiscoverySession();
  int active_count = 0;
  active_session->SetResultCallback([&](auto&) { active_count++; });

  auto passive_session = StartDiscoverySession(/*active=*/false);
  int passive_count = 0;
  passive_session->SetResultCallback([&](auto&) { passive_count++; });

  RunFor(kTestScanPeriod);
  ASSERT_TRUE(active_session);
  ASSERT_TRUE(passive_session);
  EXPECT_EQ(0, connectable_count);
  EXPECT_EQ(0, active_count);
  EXPECT_EQ(1, passive_count);
}

TEST_F(LowEnergyDiscoveryManagerTest,
       DirectedAdvertisingEventFromKnownConnectablePeer) {
  auto fake_peer = std::make_unique<FakePeer>(kAddress0,
                                              dispatcher(),
                                              /*connectable=*/true,
                                              /*scannable=*/false);
  fake_peer->set_directed_advertising_enabled(true);
  test_device()->AddPeer(std::move(fake_peer));
  Peer* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  ASSERT_TRUE(peer);

  int connectable_count = 0;
  discovery_manager()->set_peer_connectable_callback([&](Peer* callback_peer) {
    ASSERT_TRUE(callback_peer);
    EXPECT_TRUE(callback_peer->le());
    EXPECT_EQ(peer, callback_peer);
    connectable_count++;
  });
  discovery_manager()->set_scan_period(kTestScanPeriod);

  auto active_session = StartDiscoverySession();
  int active_count = 0;
  active_session->SetResultCallback([&](auto&) { active_count++; });

  auto passive_session = StartDiscoverySession(/*active=*/false);
  int passive_count = 0;
  passive_session->SetResultCallback([&](auto&) { passive_count++; });

  RunFor(kTestScanPeriod);
  ASSERT_TRUE(active_session);
  ASSERT_TRUE(passive_session);
  // Connectable callback will be notified at the start of each scan period.
  EXPECT_EQ(2, connectable_count);
  EXPECT_EQ(0, active_count);
  EXPECT_EQ(1, passive_count);
}

TEST_F(LowEnergyDiscoveryManagerTest,
       ScanResultUpgradesKnownBrEdrPeerToDualMode) {
  Peer* peer = peer_cache()->NewPeer(kAddrAlias0, /*connectable=*/true);
  ASSERT_TRUE(peer);
  ASSERT_EQ(peer, peer_cache()->FindByAddress(kAddress0));
  ASSERT_EQ(TechnologyType::kClassic, peer->technology());

  AddFakePeers();

  discovery_manager()->set_scan_period(kTestScanPeriod);

  std::unordered_set<DeviceAddress> addresses_found;
  LowEnergyDiscoverySession::PeerFoundFunction result_cb =
      [&addresses_found](const auto& peer) {
        addresses_found.insert(peer.address());
      };

  hci::DiscoveryFilter discovery_filter;
  discovery_filter.SetGeneralDiscoveryFlags();
  std::vector<hci::DiscoveryFilter> discovery_filters;
  discovery_filters.push_back(discovery_filter);

  auto session = StartDiscoverySession(/*active=*/true, discovery_filters);
  session->SetResultCallback(std::move(result_cb));

  RunUntilIdle();

  ASSERT_EQ(3u, addresses_found.size());
  EXPECT_TRUE(addresses_found.find(kAddrAlias0) != addresses_found.end());
  EXPECT_EQ(TechnologyType::kDualMode, peer->technology());
}

TEST_F(LowEnergyDiscoveryManagerTest, StartAndDisablePassiveScan) {
  ASSERT_FALSE(test_device()->le_scan_state().enabled);

  auto session = StartDiscoverySession(/*active=*/false);
  RunUntilIdle();
  EXPECT_TRUE(test_device()->le_scan_state().enabled);
  EXPECT_EQ(pw::bluetooth::emboss::LEScanType::PASSIVE,
            test_device()->le_scan_state().scan_type);
  EXPECT_FALSE(discovery_manager()->discovering());

  session.reset();
  RunUntilIdle();
  EXPECT_FALSE(test_device()->le_scan_state().enabled);
}

TEST_F(LowEnergyDiscoveryManagerTest, StartAndDisablePassiveScanQuickly) {
  ASSERT_FALSE(test_device()->le_scan_state().enabled);

  // Session will be destroyed in callback, stopping scan.
  discovery_manager()->StartDiscovery(
      /*active=*/false, {}, [&](auto cb_session) { PW_CHECK(cb_session); });
  RunUntilIdle();

  EXPECT_FALSE(test_device()->le_scan_state().enabled);
  EXPECT_EQ(2u, scan_states().size());

  // This should not result in a request to stop scan because both pending
  // requests will be processed at the same time, and second call to
  // StartDiscovery() retains its session.
  discovery_manager()->StartDiscovery(
      /*active=*/false, {}, [&](auto cb_session) { PW_CHECK(cb_session); });
  std::unique_ptr<LowEnergyDiscoverySession> session;
  discovery_manager()->StartDiscovery(
      /*active=*/false, {}, [&](auto cb_session) {
        PW_CHECK(cb_session);
        session = std::move(cb_session);
      });
  RunUntilIdle();
  EXPECT_EQ(3u, scan_states().size());

  EXPECT_TRUE(test_device()->le_scan_state().enabled);
}

TEST_F(LowEnergyDiscoveryManagerTest,
       EnablePassiveScanDuringActiveScanAndDisableActiveScanCausesDowngrade) {
  auto active_session = StartDiscoverySession();
  ASSERT_TRUE(active_session);
  ASSERT_TRUE(test_device()->le_scan_state().enabled);
  ASSERT_EQ(pw::bluetooth::emboss::LEScanType::ACTIVE,
            test_device()->le_scan_state().scan_type);

  // The scan state should transition to enabled.
  ASSERT_EQ(1u, scan_states().size());
  EXPECT_TRUE(scan_states()[0]);

  // Enabling passive scans should not disable the active scan.
  auto passive_session = StartDiscoverySession(false);
  RunUntilIdle();
  ASSERT_EQ(pw::bluetooth::emboss::LEScanType::ACTIVE,
            test_device()->le_scan_state().scan_type);
  EXPECT_TRUE(test_device()->le_scan_state().enabled);
  EXPECT_EQ(1u, scan_states().size());

  // Stopping the active session should fall back to passive scan.
  active_session = nullptr;
  RunUntilIdle();
  EXPECT_TRUE(test_device()->le_scan_state().enabled);
  EXPECT_EQ(pw::bluetooth::emboss::LEScanType::PASSIVE,
            test_device()->le_scan_state().scan_type);
  EXPECT_THAT(scan_states(), ::testing::ElementsAre(true, false, true));
}

TEST_F(LowEnergyDiscoveryManagerTest, DisablePassiveScanDuringActiveScan) {
  auto active_session = StartDiscoverySession();
  ASSERT_TRUE(active_session);
  ASSERT_TRUE(test_device()->le_scan_state().enabled);
  ASSERT_EQ(pw::bluetooth::emboss::LEScanType::ACTIVE,
            test_device()->le_scan_state().scan_type);

  // The scan state should transition to enabled.
  ASSERT_EQ(1u, scan_states().size());
  EXPECT_TRUE(scan_states()[0]);

  // Enabling passive scans should not disable the active scan.
  auto passive_session = StartDiscoverySession(false);
  RunUntilIdle();
  ASSERT_EQ(pw::bluetooth::emboss::LEScanType::ACTIVE,
            test_device()->le_scan_state().scan_type);
  EXPECT_TRUE(test_device()->le_scan_state().enabled);
  EXPECT_EQ(1u, scan_states().size());

  // Disabling the passive scan should not disable the active scan.
  passive_session.reset();
  RunUntilIdle();
  ASSERT_EQ(pw::bluetooth::emboss::LEScanType::ACTIVE,
            test_device()->le_scan_state().scan_type);
  EXPECT_TRUE(test_device()->le_scan_state().enabled);
  EXPECT_EQ(1u, scan_states().size());

  // Stopping the active session should stop scans.
  active_session = nullptr;
  RunUntilIdle();
  EXPECT_FALSE(test_device()->le_scan_state().enabled);
  EXPECT_THAT(scan_states(), ::testing::ElementsAre(true, false));
}

TEST_F(LowEnergyDiscoveryManagerTest, StartActiveScanDuringPassiveScan) {
  auto passive_session = StartDiscoverySession(false);
  RunUntilIdle();
  ASSERT_TRUE(test_device()->le_scan_state().enabled);
  ASSERT_EQ(pw::bluetooth::emboss::LEScanType::PASSIVE,
            test_device()->le_scan_state().scan_type);

  // The scan state should transition to enabled.
  ASSERT_EQ(1u, scan_states().size());
  EXPECT_TRUE(scan_states()[0]);

  // Starting discovery should turn off the passive scan and initiate an active
  // scan.
  auto active_session = StartDiscoverySession();
  EXPECT_TRUE(active_session);
  EXPECT_TRUE(test_device()->le_scan_state().enabled);
  EXPECT_EQ(pw::bluetooth::emboss::LEScanType::ACTIVE,
            test_device()->le_scan_state().scan_type);
  EXPECT_THAT(scan_states(), ::testing::ElementsAre(true, false, true));
}

TEST_F(LowEnergyDiscoveryManagerTest,
       DISABLED_StartScanDuringOffloadedFilters) {
  SetupDiscoveryManager(
      /*extended=*/false,
      {true,
       8,
       hci::AdvertisingPacketFilter::Config::DeliveryMode::kImmediate});

  auto session_a = StartDiscoverySession(false);
  RunUntilIdle();
  ASSERT_TRUE(test_device()->le_scan_state().enabled);

  // The scan state should transition to enabled.
  ASSERT_EQ(1u, scan_states().size());
  EXPECT_TRUE(scan_states()[0]);

  // starting another discovery session while offloading is enabled should cause
  // us to restart the scan so the new filters can take effect in the Controller
  hci::DiscoveryFilter filter;
  filter.set_name_substring("bort");
  auto session_b = StartDiscoverySession(false, {filter});

  EXPECT_THAT(scan_states(), ::testing::ElementsAre(true, false, true));
}

TEST_F(LowEnergyDiscoveryManagerTest, StartActiveScanWhileStartingPassiveScan) {
  std::unique_ptr<LowEnergyDiscoverySession> passive_session;
  discovery_manager()->StartDiscovery(
      /*active=*/false, {}, [&](auto cb_session) {
        PW_CHECK(cb_session);
        passive_session = std::move(cb_session);
      });
  ASSERT_FALSE(passive_session);

  std::unique_ptr<LowEnergyDiscoverySession> active_session;
  discovery_manager()->StartDiscovery(
      /*active=*/true, {}, [&](auto cb_session) {
        PW_CHECK(cb_session);
        active_session = std::move(cb_session);
      });
  ASSERT_FALSE(active_session);

  // Scan should not be enabled yet.
  EXPECT_FALSE(test_device()->le_scan_state().enabled);
  EXPECT_TRUE(scan_states().empty());

  // Process all the requests. We should observe multiple state transitions:
  // -> enabled (passive) -> disabled -> enabled (active)
  RunUntilIdle();
  ASSERT_TRUE(test_device()->le_scan_state().enabled);
  EXPECT_EQ(pw::bluetooth::emboss::LEScanType::ACTIVE,
            test_device()->le_scan_state().scan_type);
  EXPECT_THAT(scan_states(), ::testing::ElementsAre(true, false, true));
}

// Emulate a number of connectable and non-connectable advertisers in both
// undirected connectable and directed connectable modes. This test is to ensure
// that the only peers notified during a passive scan are from connectable peers
// that are already in the cache.
TEST_F(LowEnergyDiscoveryManagerTest,
       PeerConnectableCallbackOnlyHandlesEventsFromKnownConnectableDevices) {
  // Address 0: undirected connectable; added to cache below
  {
    auto peer = std::make_unique<FakePeer>(kAddress0,
                                           dispatcher(),
                                           /*connectable=*/true,
                                           /*scannable=*/true);
    test_device()->AddPeer(std::move(peer));
  }
  // Address 1: undirected connectable; NOT in cache
  {
    auto peer = std::make_unique<FakePeer>(kAddress1,
                                           dispatcher(),
                                           /*connectable=*/true,
                                           /*scannable=*/true);
    test_device()->AddPeer(std::move(peer));
  }
  // Address 2: not connectable; added to cache below
  {
    auto peer = std::make_unique<FakePeer>(kAddress2,
                                           dispatcher(),
                                           /*connectable=*/false,
                                           /*scannable=*/false);
    test_device()->AddPeer(std::move(peer));
  }
  // Address 3: not connectable but directed advertising (NOTE: although a
  // directed advertising PDU is inherently connectable, it is theoretically
  // possible for the peer_cache() to be in this state, even if unlikely in
  // practice).
  //
  // added to cache below
  {
    auto peer = std::make_unique<FakePeer>(kAddress3,
                                           dispatcher(),
                                           /*connectable=*/false,
                                           /*scannable=*/false);
    peer->set_directed_advertising_enabled(true);
    test_device()->AddPeer(std::move(peer));
  }
  // Address 4: directed connectable; added to cache below
  {
    auto peer = std::make_unique<FakePeer>(kAddress4,
                                           dispatcher(),
                                           /*connectable=*/true,
                                           /*scannable=*/false);
    peer->set_directed_advertising_enabled(true);
    test_device()->AddPeer(std::move(peer));
  }
  // Address 5: directed connectable; NOT in cache
  {
    auto peer = std::make_unique<FakePeer>(kAddress5,
                                           dispatcher(),
                                           /*connectable=*/true,
                                           /*scannable=*/false);
    peer->set_directed_advertising_enabled(true);
    test_device()->AddPeer(std::move(peer));
  }

  // Add cache entries for addresses 0, 2, 3, and 4. The callback should only
  // run for addresses 0 and 4 as the only known connectable peers. All other
  // advertisements should be ignored.
  auto address0_id =
      peer_cache()->NewPeer(kAddress0, /*connectable=*/true)->identifier();
  peer_cache()->NewPeer(kAddress2, /*connectable=*/false);
  peer_cache()->NewPeer(kAddress3, /*connectable=*/false);
  auto address4_id =
      peer_cache()->NewPeer(kAddress4, /*connectable=*/true)->identifier();
  EXPECT_EQ(4u, peer_cache()->count());

  int count = 0;
  discovery_manager()->set_peer_connectable_callback([&](Peer* peer) {
    ASSERT_TRUE(peer);
    auto id = peer->identifier();
    count++;
    EXPECT_TRUE(id == address0_id || id == address4_id) << id.ToString();
  });
  auto session = StartDiscoverySession(/*active=*/false);
  RunUntilIdle();
  EXPECT_EQ(2, count);

  // No new remote peer cache entries should have been created.
  EXPECT_EQ(4u, peer_cache()->count());
}

TEST_F(LowEnergyDiscoveryManagerTest, PassiveScanPeriodRestart) {
  discovery_manager()->set_scan_period(kTestScanPeriod);
  auto session = StartDiscoverySession(/*active=*/false);

  // The scan state should transition to enabled.
  RunUntilIdle();
  EXPECT_TRUE(scan_enabled());
  ASSERT_EQ(1u, scan_states().size());
  EXPECT_TRUE(test_device()->le_scan_state().enabled);

  // End the scan period by advancing time.
  RunFor(kTestScanPeriod);
  EXPECT_TRUE(test_device()->le_scan_state().enabled);
  EXPECT_EQ(pw::bluetooth::emboss::LEScanType::PASSIVE,
            test_device()->le_scan_state().scan_type);
  EXPECT_THAT(scan_states(), ::testing::ElementsAre(true, false, true));
}

TEST_F(
    LowEnergyDiscoveryManagerTest,
    PauseActiveDiscoveryTwiceKeepsScanningDisabledUntilBothPauseTokensDestroyed) {
  auto session = StartDiscoverySession();
  EXPECT_TRUE(scan_enabled());

  std::optional<PauseToken> pause_0 = discovery_manager()->PauseDiscovery();
  RunUntilIdle();
  EXPECT_FALSE(scan_enabled());
  EXPECT_TRUE(discovery_manager()->discovering());

  std::optional<PauseToken> pause_1 = discovery_manager()->PauseDiscovery();
  RunUntilIdle();
  EXPECT_FALSE(scan_enabled());
  EXPECT_TRUE(discovery_manager()->discovering());

  pause_0.reset();
  RunUntilIdle();
  EXPECT_FALSE(scan_enabled());
  EXPECT_TRUE(discovery_manager()->discovering());

  pause_1.reset();
  RunUntilIdle();
  EXPECT_TRUE(scan_enabled());
  EXPECT_TRUE(discovery_manager()->discovering());
}

TEST_F(LowEnergyDiscoveryManagerTest, EnablePassiveScanAfterPausing) {
  std::optional<PauseToken> pause = discovery_manager()->PauseDiscovery();
  RunUntilIdle();
  EXPECT_FALSE(scan_enabled());

  std::unique_ptr<LowEnergyDiscoverySession> session;
  discovery_manager()->StartDiscovery(
      /*active=*/false, {}, [&](auto cb_session) {
        session = std::move(cb_session);
      });
  RunUntilIdle();
  EXPECT_FALSE(scan_enabled());
  EXPECT_FALSE(session);

  pause.reset();
  RunUntilIdle();
  EXPECT_TRUE(scan_enabled());
}

TEST_F(LowEnergyDiscoveryManagerTest, StartActiveScanAfterPausing) {
  std::optional<PauseToken> pause = discovery_manager()->PauseDiscovery();
  RunUntilIdle();
  EXPECT_FALSE(scan_enabled());

  std::unique_ptr<LowEnergyDiscoverySession> session;
  discovery_manager()->StartDiscovery(
      /*active=*/true, {}, [&](auto cb_session) {
        session = std::move(cb_session);
      });
  RunUntilIdle();
  EXPECT_FALSE(scan_enabled());
  EXPECT_FALSE(session);

  pause.reset();
  RunUntilIdle();
  EXPECT_TRUE(scan_enabled());
  EXPECT_TRUE(session);
}

TEST_F(LowEnergyDiscoveryManagerTest, PauseDiscoveryJustBeforeScanComplete) {
  discovery_manager()->set_scan_period(kTestScanPeriod);

  auto session = StartDiscoverySession();
  EXPECT_TRUE(scan_enabled());

  // Pause discovery in FakeController scan state callback to ensure it is
  // called just before kComplete status is received. This will be the 2nd scan
  // state change because it is started above and then stopped by the scan
  // period ending below.
  std::optional<PauseToken> pause;
  set_scan_state_handler(
      2, [this, &pause]() { pause = discovery_manager()->PauseDiscovery(); });

  RunFor(kTestScanPeriod);
  EXPECT_TRUE(pause.has_value());
  EXPECT_EQ(scan_states().size(), 2u);
  EXPECT_FALSE(scan_enabled());
}

TEST_F(LowEnergyDiscoveryManagerTest, PauseDiscoveryJustBeforeScanStopped) {
  auto session = StartDiscoverySession();
  EXPECT_TRUE(scan_enabled());

  // Pause discovery in FakeController scan state callback to ensure it is
  // called just before kStopped status is received. This will be the 2nd scan
  // state change because it is started above and then stopped by the session
  // being destroyed below.
  std::optional<PauseToken> pause;
  set_scan_state_handler(
      2, [this, &pause]() { pause = discovery_manager()->PauseDiscovery(); });

  session.reset();
  RunUntilIdle();
  EXPECT_TRUE(pause.has_value());
  EXPECT_EQ(scan_states().size(), 2u);
  EXPECT_FALSE(scan_enabled());
}

TEST_F(LowEnergyDiscoveryManagerTest, PauseJustBeforeScanActive) {
  // Pause discovery in FakeController scan state callback to ensure it is
  // called just before kActive status is received. This will be the first scan
  // state change.
  std::optional<PauseToken> pause;
  set_scan_state_handler(
      1, [this, &pause]() { pause = discovery_manager()->PauseDiscovery(); });

  std::unique_ptr<LowEnergyDiscoverySession> session;
  discovery_manager()->StartDiscovery(
      /*active=*/true, {}, [&](auto cb_session) {
        session = std::move(cb_session);
      });

  // The scan should be canceled.
  RunUntilIdle();
  EXPECT_FALSE(session);
  EXPECT_TRUE(pause.has_value());
  EXPECT_EQ(scan_states().size(), 2u);
  EXPECT_FALSE(scan_enabled());
  EXPECT_FALSE(discovery_manager()->discovering());

  // Resume discovery.
  pause.reset();
  RunUntilIdle();
  EXPECT_TRUE(session);
  EXPECT_TRUE(scan_enabled());
  EXPECT_TRUE(discovery_manager()->discovering());
}

TEST_F(LowEnergyDiscoveryManagerTest, PauseJustBeforeScanPassive) {
  // Pause discovery in FakeController scan state callback to ensure it is
  // called just before kPassive status is received. This will be the first scan
  // state change.
  std::optional<PauseToken> pause;
  set_scan_state_handler(
      1, [this, &pause]() { pause = discovery_manager()->PauseDiscovery(); });

  std::unique_ptr<LowEnergyDiscoverySession> session;
  discovery_manager()->StartDiscovery(
      /*active=*/false, {}, [&](auto cb_session) {
        session = std::move(cb_session);
      });

  // The scan should be canceled.
  RunUntilIdle();
  EXPECT_FALSE(session);
  EXPECT_TRUE(pause.has_value());
  EXPECT_EQ(scan_states().size(), 2u);
  EXPECT_FALSE(scan_enabled());

  // Resume scan.
  pause.reset();
  RunUntilIdle();
  EXPECT_TRUE(scan_enabled());
}

TEST_F(LowEnergyDiscoveryManagerTest,
       StartActiveScanWhilePassiveScanStoppingBetweenScanPeriods) {
  discovery_manager()->set_scan_period(kTestScanPeriod);

  auto passive_session = StartDiscoverySession(/*active=*/false);

  std::unique_ptr<LowEnergyDiscoverySession> active_session;
  set_scan_state_handler(2, [this, &active_session]() {
    discovery_manager()->StartDiscovery(
        /*active=*/true, {}, [&active_session](auto session) {
          active_session = std::move(session);
        });
  });
  RunFor(kTestScanPeriod);
  EXPECT_TRUE(test_device()->le_scan_state().enabled);
  EXPECT_EQ(pw::bluetooth::emboss::LEScanType::ACTIVE,
            test_device()->le_scan_state().scan_type);
  EXPECT_THAT(scan_states(), ::testing::ElementsAre(true, false, true));
}

TEST_F(LowEnergyDiscoveryManagerTest,
       StopSessionInsideOfResultCallbackDoesNotCrash) {
  auto session = StartDiscoverySession(/*active=*/false);
  auto result_cb = [&session](const auto&) { session->Stop(); };
  session->SetResultCallback(std::move(result_cb));
  RunUntilIdle();

  AddFakePeers();
  RunUntilIdle();
}

TEST_F(LowEnergyDiscoveryManagerTest,
       PeerChangesFromNonConnectableToConnectable) {
  test_device()->AddPeer(std::make_unique<FakePeer>(
      kAddress0, dispatcher(), /*connectable=*/false));

  std::unique_ptr<LowEnergyDiscoverySession> session;
  discovery_manager()->StartDiscovery(
      /*active=*/true, {}, [&session](auto cb_session) {
        session = std::move(cb_session);
      });

  RunUntilIdle();
  EXPECT_TRUE(scan_enabled());
  auto peer = peer_cache()->FindByAddress(kAddress0);
  ASSERT_TRUE(peer);
  EXPECT_FALSE(peer->connectable());

  // Make peer connectable.
  test_device()->RemovePeer(kAddress0);
  test_device()->AddPeer(std::make_unique<FakePeer>(
      kAddress0, dispatcher(), /*connectable=*/true));

  RunUntilIdle();
  peer = peer_cache()->FindByAddress(kAddress0);
  ASSERT_TRUE(peer);
  EXPECT_TRUE(peer->connectable());

  // Ensure peer stays connectable after non-connectable advertisement.
  test_device()->RemovePeer(kAddress0);
  test_device()->AddPeer(std::make_unique<FakePeer>(
      kAddress0, dispatcher(), /*connectable=*/false));

  RunUntilIdle();
  peer = peer_cache()->FindByAddress(kAddress0);
  ASSERT_TRUE(peer);
  EXPECT_TRUE(peer->connectable());
}

#ifndef NINSPECT
TEST_F(LowEnergyDiscoveryManagerTest, Inspect) {
  // Ensure node exists before testing properties.
  ASSERT_THAT(InspectHierarchy(),
              AllOf(ChildrenMatch(ElementsAre(NodeMatches(
                  AllOf(NameMatches(std::string(kInspectNodeName))))))));
  EXPECT_THAT(InspectProperties(),
              UnorderedElementsAre(StringIs("state", "Idle"),
                                   IntIs("paused", 0),
                                   UintIs("failed_count", 0u),
                                   DoubleIs("scan_interval_ms", 0.0),
                                   DoubleIs("scan_window_ms", 0.0)));

  std::unique_ptr<LowEnergyDiscoverySession> passive_session;
  discovery_manager()->StartDiscovery(
      /*active=*/false, {}, [&](auto cb_session) {
        PW_CHECK(cb_session);
        passive_session = std::move(cb_session);
      });
  EXPECT_THAT(InspectProperties(),
              ::testing::IsSupersetOf(
                  {StringIs("state", "Starting"),
                   DoubleIs("scan_interval_ms", ::testing::Gt(0.0)),
                   DoubleIs("scan_window_ms", ::testing::Gt(0.0))}));

  RunUntilIdle();
  EXPECT_THAT(InspectProperties(),
              ::testing::IsSupersetOf(
                  {StringIs("state", "Passive"),
                   DoubleIs("scan_interval_ms", ::testing::Gt(0.0)),
                   DoubleIs("scan_window_ms", ::testing::Gt(0.0))}));

  {
    auto pause_token = discovery_manager()->PauseDiscovery();
    EXPECT_THAT(InspectProperties(),
                ::testing::IsSupersetOf(
                    {StringIs("state", "Stopping"), IntIs("paused", 1)}));
  }

  auto active_session = StartDiscoverySession();
  EXPECT_THAT(InspectProperties(),
              ::testing::IsSupersetOf(
                  {StringIs("state", "Active"),
                   DoubleIs("scan_interval_ms", ::testing::Gt(0.0)),
                   DoubleIs("scan_window_ms", ::testing::Gt(0.0))}));

  passive_session.reset();
  active_session.reset();
  EXPECT_THAT(InspectProperties(),
              ::testing::IsSupersetOf({StringIs("state", "Stopping")}));
  RunUntilIdle();
  EXPECT_THAT(InspectProperties(),
              ::testing::IsSupersetOf({StringIs("state", "Idle")}));

  // Cause discovery to fail.
  test_device()->SetDefaultResponseStatus(
      hci_spec::kLESetScanEnable,
      pw::bluetooth::emboss::StatusCode::COMMAND_DISALLOWED);
  discovery_manager()->StartDiscovery(
      /*active=*/true, {}, [](auto session) { EXPECT_FALSE(session); });
  RunUntilIdle();
  EXPECT_THAT(InspectProperties(),
              ::testing::IsSupersetOf({UintIs("failed_count", 1u)}));
}
#endif  // NINSPECT

TEST_F(LowEnergyDiscoveryManagerTest, SetResultCallbackIgnoresRemovedPeers) {
  auto fake_peer_0 = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer_0));
  Peer* peer_0 = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);
  PeerId peer_id_0 = peer_0->identifier();

  auto fake_peer_1 = std::make_unique<FakePeer>(kAddress1, dispatcher());
  test_device()->AddPeer(std::move(fake_peer_1));
  Peer* peer_1 = peer_cache()->NewPeer(kAddress1, /*connectable=*/true);
  PeerId peer_id_1 = peer_1->identifier();

  // Start active session so that results get cached.
  auto session = StartDiscoverySession();

  std::unordered_map<PeerId, int> result_counts;
  session->SetResultCallback(
      [&](const Peer& peer) { result_counts[peer.identifier()]++; });
  RunUntilIdle();
  EXPECT_EQ(result_counts[peer_id_0], 1);
  EXPECT_EQ(result_counts[peer_id_1], 1);

  // Remove peer_0 to make the cached result stale. The result callback should
  // not be called again for peer_0.
  ASSERT_TRUE(peer_cache()->RemoveDisconnectedPeer(peer_0->identifier()));
  session->SetResultCallback(
      [&](const Peer& peer) { result_counts[peer.identifier()]++; });
  RunUntilIdle();
  EXPECT_EQ(result_counts[peer_id_0], 1);
  EXPECT_EQ(result_counts[peer_id_1], 2);
}

TEST_F(LowEnergyDiscoveryManagerTest, NewSessionJoinsOngoingScan) {
  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));
  Peer* peer = peer_cache()->NewPeer(kAddress0, /*connectable=*/true);

  // Start active session so that results get cached.
  auto unused_session = StartDiscoverySession();

  auto session = StartDiscoverySession();
  std::unordered_set<PeerId> results;
  session->SetResultCallback(
      [&](const Peer& peer) { results.insert(peer.identifier()); });
  RunUntilIdle();
  ASSERT_EQ(1u, results.size());
  EXPECT_EQ(peer->identifier(), *results.begin());
}

// Client code may be multithreaded and use mutexes while calling
// LowEnergyDiscoverySession::SetPacketFilters(...). Enusre that we don't call
// the peer found callback in the same call stack to avoid client bugs
// (e.g. deadlock).
TEST_F(LowEnergyDiscoveryManagerTest, SetResultCallbackPostsDiscoveryResults) {
  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));
  peer_cache()->NewPeer(kAddress0, /*connectable=*/true);

  // Start active session so that results get cached.
  auto session = StartDiscoverySession();

  bool callback_called = false;
  session->SetResultCallback(
      [&](const Peer& /*peer*/) { callback_called = true; });

  ASSERT_FALSE(callback_called);
  RunUntilIdle();
  ASSERT_TRUE(callback_called);
}

// Information only found in the extended data advertisement is properly
// translated from scan results to peer fields.
TEST_F(LowEnergyDiscoveryManagerTest, LeExtendedDataIsPopulated) {
  SetupDiscoveryManager(/*extended=*/true);
  uint8_t kAdvertisingSid = 0x08;
  uint16_t kPeriodicAdvertisingInterval = 0xfedc;
  auto fake_peer = std::make_unique<FakePeer>(kAddress0, dispatcher());
  fake_peer->set_advertising_sid(kAdvertisingSid);
  fake_peer->set_periodic_advertising_interval(kPeriodicAdvertisingInterval);
  test_device()->AddPeer(std::move(fake_peer));

  auto session = StartDiscoverySession();
  bool peer_seen = false;
  session->SetResultCallback([&](const Peer& peer) {
    ASSERT_EQ(peer.address(), kAddress0);
    peer_seen = true;
    EXPECT_EQ(peer.le()->advertising_sid(), kAdvertisingSid);
    EXPECT_EQ(peer.le()->periodic_advertising_interval(),
              kPeriodicAdvertisingInterval);
  });
  RunUntilIdle();
  EXPECT_TRUE(peer_seen);
}

}  // namespace
}  // namespace bt::gap
