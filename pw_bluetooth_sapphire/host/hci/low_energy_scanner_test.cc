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

#include <unordered_set>

#include "pw_bluetooth_sapphire/internal/host/hci/extended_low_energy_scanner.h"
#include "pw_bluetooth_sapphire/internal/host/hci/fake_local_address_delegate.h"
#include "pw_bluetooth_sapphire/internal/host/hci/legacy_low_energy_scanner.h"
#include "pw_bluetooth_sapphire/internal/host/testing/controller_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_controller.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_peer.h"

// LowEnergyScanner has many potential subclasses (e.g. LegacyLowEnergyScanner,
// ExtendedLowEnergyScanner, etc). The unique features of these subclasses are
// tested individually in their own unittest files. However, there are some
// common features that all LowEnergyScanners should follow. This test file
// implements a type parameterized test to exercise those common features.
//
// If you add a new subclass of LowEnergyScanner in the future, make sure to add
// its type to the list of types below (in the TYPED_TEST_SUITE) so that its
// common features are exercised as well.

namespace bt::hci {

using bt::testing::FakeController;
using bt::testing::FakePeer;
using TestingBase = bt::testing::FakeDispatcherControllerTest<FakeController>;

constexpr pw::chrono::SystemClock::duration kScanPeriod =
    std::chrono::seconds(10);
constexpr pw::chrono::SystemClock::duration kPwScanPeriod =
    std::chrono::seconds(10);
constexpr pw::chrono::SystemClock::duration kScanResponseTimeout =
    std::chrono::seconds(2);
constexpr pw::chrono::SystemClock::duration kPwScanResponseTimeout =
    std::chrono::seconds(2);

// The unit tests below assume that the scan period is longer than the scan
// response timeout when exercising timeout expiration.
static_assert(kScanResponseTimeout < kScanPeriod,
              "expected a smaller scan response timeout for testing");

const StaticByteBuffer kPlainAdvDataBytes(
    5, bt::DataType::kCompleteLocalName, 'T', 'e', 's', 't');
const StaticByteBuffer kPlainScanRspBytes(
    5, bt::DataType::kCompleteLocalName, 'D', 'a', 't', 'a');

const std::string kPlainAdvData = kPlainAdvDataBytes.ToString();
const std::string kPlainScanRsp = kPlainScanRspBytes.ToString();
const std::string kAdvDataAndScanRsp = kPlainAdvData + kPlainScanRsp;

const DeviceAddress kPublicAddress1(DeviceAddress::Type::kLEPublic, {1});
const DeviceAddress kPublicAddress2(DeviceAddress::Type::kLEPublic, {2});

const DeviceAddress kRandomAddress1(DeviceAddress::Type::kLERandom, {3});
const DeviceAddress kRandomAddress2(DeviceAddress::Type::kLERandom, {4});
const DeviceAddress kRandomAddress3(DeviceAddress::Type::kLERandom, {5});
const DeviceAddress kRandomAddress4(DeviceAddress::Type::kLERandom, {6});

template <typename T>
class LowEnergyScannerTest : public TestingBase,
                             public LowEnergyScanner::Delegate {
 public:
  LowEnergyScannerTest() = default;
  ~LowEnergyScannerTest() override = default;

 protected:
  void SetUp() override {
    TestingBase::SetUp();

    FakeController::Settings settings;
    settings.ApplyLegacyLEConfig();
    this->test_device()->set_settings(settings);

    CreateScanner<T>();
  }

  void TearDown() override {
    scanner_ = nullptr;
    this->test_device()->Stop();
    TestingBase::TearDown();
  }

  template <typename U>
  void CreateScanner(bool enable_offloading = false, uint8_t max_filters = 4) {
    scanner_ = std::unique_ptr<U>(
        CreateScannerInternal(enable_offloading, max_filters));
    scanner_->SetPacketFilters(0, {});
    scanner_->set_delegate(this);
  }

  template <bool same = std::is_same_v<T, ExtendedLowEnergyScanner>>
  std::enable_if_t<same, ExtendedLowEnergyScanner>* CreateScannerInternal(
      bool enable_offloading = false, uint8_t max_filters = 4) {
    return new ExtendedLowEnergyScanner(
        fake_address_delegate(),
        {enable_offloading,
         max_filters,
         AdvertisingPacketFilter::Config::DeliveryMode::kImmediate},
        transport()->GetWeakPtr(),
        dispatcher());
  }

  template <bool same = std::is_same_v<T, LegacyLowEnergyScanner>>
  std::enable_if_t<same, LegacyLowEnergyScanner>* CreateScannerInternal(
      bool enable_offloading = false, uint8_t max_filters = 4) {
    return new LegacyLowEnergyScanner(
        fake_address_delegate(),
        {enable_offloading,
         max_filters,
         AdvertisingPacketFilter::Config::DeliveryMode::kImmediate},
        transport()->GetWeakPtr(),
        dispatcher());
  }

  void EnableOffloading(uint8_t max_filters = 4) {
    CreateScanner<T>(true, max_filters);
  }

  void DisableOffloading() { CreateScanner<T>(false); }

  using PeerFoundCallback = fit::function<void(
      const std::unordered_set<uint16_t>&, const LowEnergyScanResult&)>;
  void set_peer_found_callback(PeerFoundCallback cb) {
    peer_found_cb_ = std::move(cb);
  }

  using DirectedAdvCallback = fit::function<void(const LowEnergyScanResult&)>;
  void set_directed_adv_callback(DirectedAdvCallback cb) {
    directed_adv_cb_ = std::move(cb);
  }

  bool StartScan(bool active,
                 pw::chrono::SystemClock::duration period =
                     LowEnergyScanner::kPeriodInfinite) {
    LowEnergyScanner::ScanOptions options{
        .active = active,
        .filter_duplicates = true,
        .period = period,
        .scan_response_timeout = kPwScanResponseTimeout};
    return scanner()->StartScan(
        options, [this](auto status) { last_scan_status_ = status; });
  }

  // LowEnergyScanner::Delegate override:
  void OnPeerFound(const std::unordered_set<uint16_t>& scan_ids,
                   const LowEnergyScanResult& result) override {
    if (peer_found_cb_) {
      peer_found_cb_(scan_ids, result);
    }
  }

  // LowEnergyScanner::Observer override:
  void OnDirectedAdvertisement(const LowEnergyScanResult& result) override {
    if (directed_adv_cb_) {
      directed_adv_cb_(result);
    }
  }

  // Adds 6 fake peers using kAddress[0-5] above.
  void AddFakePeers() {
    // Generates ADV_IND
    auto fake_peer =
        std::make_unique<FakePeer>(kPublicAddress1, dispatcher(), true, true);
    fake_peer->set_advertising_data(kPlainAdvDataBytes);
    fake_peer->set_scan_response(kPlainScanRspBytes);
    test_device()->AddPeer(std::move(fake_peer));

    // Generates ADV_SCAN_IND
    fake_peer =
        std::make_unique<FakePeer>(kRandomAddress1, dispatcher(), false, true);
    fake_peer->set_advertising_data(kPlainAdvDataBytes);
    fake_peer->set_scan_response(kPlainScanRspBytes);
    test_device()->AddPeer(std::move(fake_peer));

    // Generates ADV_IND
    fake_peer =
        std::make_unique<FakePeer>(kPublicAddress2, dispatcher(), true, true);
    fake_peer->set_advertising_data(kPlainAdvDataBytes);
    fake_peer->set_scan_response(DynamicByteBuffer());
    test_device()->AddPeer(std::move(fake_peer));

    // Generates ADV_IND
    fake_peer =
        std::make_unique<FakePeer>(kRandomAddress2, dispatcher(), true, true);
    fake_peer->set_advertising_data(kPlainAdvDataBytes);
    fake_peer->set_scan_response(kPlainScanRspBytes);
    test_device()->AddPeer(std::move(fake_peer));

    // Generates ADV_IND, a scan response is never sent even though ADV_IND is
    // scannable.
    fake_peer =
        std::make_unique<FakePeer>(kRandomAddress3, dispatcher(), true, false);
    fake_peer->set_advertising_data(kPlainAdvDataBytes);
    test_device()->AddPeer(std::move(fake_peer));

    // Generates ADV_NONCONN_IND
    fake_peer =
        std::make_unique<FakePeer>(kRandomAddress4, dispatcher(), false, false);
    fake_peer->set_advertising_data(kPlainAdvDataBytes);
    test_device()->AddPeer(std::move(fake_peer));
  }

  LowEnergyScanner* scanner() const { return scanner_.get(); }
  FakeLocalAddressDelegate* fake_address_delegate() {
    return &fake_address_delegate_;
  }

  LowEnergyScanner::ScanStatus last_scan_status() const {
    return last_scan_status_;
  }

 private:
  PeerFoundCallback peer_found_cb_;
  DirectedAdvCallback directed_adv_cb_;
  FakeLocalAddressDelegate fake_address_delegate_{dispatcher()};
  std::unique_ptr<LowEnergyScanner> scanner_;

  LowEnergyScanner::ScanStatus last_scan_status_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(LowEnergyScannerTest);
};

using Implementations =
    ::testing::Types<LegacyLowEnergyScanner, ExtendedLowEnergyScanner>;
TYPED_TEST_SUITE(LowEnergyScannerTest, Implementations);

TYPED_TEST(LowEnergyScannerTest, StartScanHCIErrors) {
  EXPECT_TRUE(this->scanner()->IsIdle());
  EXPECT_FALSE(this->scanner()->IsScanning());
  EXPECT_FALSE(this->test_device()->le_scan_state().enabled);

  // Set Scan Parameters will fail.
  this->test_device()->SetDefaultResponseStatus(
      hci_spec::kLESetScanParameters,
      pw::bluetooth::emboss::StatusCode::HARDWARE_FAILURE);
  this->test_device()->SetDefaultResponseStatus(
      hci_spec::kLESetExtendedScanParameters,
      pw::bluetooth::emboss::StatusCode::HARDWARE_FAILURE);
  EXPECT_EQ(0, this->test_device()->le_scan_state().scan_interval);

  EXPECT_TRUE(this->StartScan(false));
  EXPECT_EQ(LowEnergyScanner::State::kInitiating, this->scanner()->state());

  // Calling StartScan() should fail as the state is not kIdle.
  EXPECT_FALSE(this->StartScan(false));
  this->RunUntilIdle();

  // Status should be failure and the scan parameters shouldn't have applied.
  EXPECT_EQ(LowEnergyScanner::ScanStatus::kFailed, this->last_scan_status());
  EXPECT_EQ(0, this->test_device()->le_scan_state().scan_interval);
  EXPECT_FALSE(this->test_device()->le_scan_state().enabled);
  EXPECT_TRUE(this->scanner()->IsIdle());
  EXPECT_FALSE(this->scanner()->IsScanning());

  // Set Scan Parameters will succeed but Set Scan Enable will fail.
  this->test_device()->ClearDefaultResponseStatus(
      hci_spec::kLESetScanParameters);
  this->test_device()->ClearDefaultResponseStatus(
      hci_spec::kLESetExtendedScanParameters);
  this->test_device()->SetDefaultResponseStatus(
      hci_spec::kLESetScanEnable,
      pw::bluetooth::emboss::StatusCode::HARDWARE_FAILURE);
  this->test_device()->SetDefaultResponseStatus(
      hci_spec::kLESetExtendedScanEnable,
      pw::bluetooth::emboss::StatusCode::HARDWARE_FAILURE);

  EXPECT_TRUE(this->StartScan(false));
  EXPECT_EQ(LowEnergyScanner::State::kInitiating, this->scanner()->state());
  this->RunUntilIdle();

  // Status should be failure but the scan parameters should have applied.
  EXPECT_EQ(LowEnergyScanner::ScanStatus::kFailed, this->last_scan_status());
  EXPECT_EQ(hci_spec::defaults::kLEScanInterval,
            this->test_device()->le_scan_state().scan_interval);
  EXPECT_EQ(hci_spec::defaults::kLEScanWindow,
            this->test_device()->le_scan_state().scan_window);
  EXPECT_EQ(pw::bluetooth::emboss::LEScanFilterPolicy::BASIC_UNFILTERED,
            this->test_device()->le_scan_state().filter_policy);
  EXPECT_FALSE(this->test_device()->le_scan_state().enabled);
  EXPECT_TRUE(this->scanner()->IsIdle());
  EXPECT_FALSE(this->scanner()->IsScanning());
}

TYPED_TEST(LowEnergyScannerTest, StartScan) {
  EXPECT_TRUE(this->scanner()->IsIdle());
  EXPECT_FALSE(this->scanner()->IsScanning());
  EXPECT_FALSE(this->test_device()->le_scan_state().enabled);

  EXPECT_TRUE(this->StartScan(true, kPwScanPeriod));
  EXPECT_EQ(LowEnergyScanner::State::kInitiating, this->scanner()->state());
  this->RunUntilIdle();

  // Scan should have started.
  EXPECT_EQ(LowEnergyScanner::ScanStatus::kActive, this->last_scan_status());
  EXPECT_EQ(hci_spec::defaults::kLEScanInterval,
            this->test_device()->le_scan_state().scan_interval);
  EXPECT_EQ(hci_spec::defaults::kLEScanWindow,
            this->test_device()->le_scan_state().scan_window);
  EXPECT_EQ(pw::bluetooth::emboss::LEScanFilterPolicy::BASIC_UNFILTERED,
            this->test_device()->le_scan_state().filter_policy);
  EXPECT_EQ(pw::bluetooth::emboss::LEScanType::ACTIVE,
            this->test_device()->le_scan_state().scan_type);
  EXPECT_TRUE(this->test_device()->le_scan_state().filter_duplicates);
  EXPECT_TRUE(this->test_device()->le_scan_state().enabled);
  EXPECT_EQ(LowEnergyScanner::State::kActiveScanning, this->scanner()->state());
  EXPECT_TRUE(this->scanner()->IsScanning());

  // Calling StartScan should fail as a scan is already in progress.
  EXPECT_FALSE(this->StartScan(true));

  // After 10 s (kScanPeriod) the scan should stop by itself.
  this->RunFor(kScanPeriod);

  EXPECT_EQ(LowEnergyScanner::ScanStatus::kComplete, this->last_scan_status());
  EXPECT_FALSE(this->test_device()->le_scan_state().enabled);
  EXPECT_TRUE(this->scanner()->IsIdle());
  EXPECT_FALSE(this->scanner()->IsScanning());
}

TYPED_TEST(LowEnergyScannerTest, StopScan) {
  EXPECT_TRUE(this->scanner()->IsIdle());
  EXPECT_FALSE(this->scanner()->IsScanning());
  EXPECT_FALSE(this->test_device()->le_scan_state().enabled);

  // Calling StopScan should fail while a scan is not in progress.
  EXPECT_FALSE(this->scanner()->StopScan());

  // Pass a long scan period value. This should not matter as we will terminate
  // the scan directly.
  EXPECT_TRUE(this->StartScan(true, kPwScanPeriod * 10u));
  EXPECT_EQ(LowEnergyScanner::State::kInitiating, this->scanner()->state());
  this->RunUntilIdle();

  // Scan should have started.
  EXPECT_EQ(LowEnergyScanner::ScanStatus::kActive, this->last_scan_status());
  EXPECT_TRUE(this->test_device()->le_scan_state().enabled);
  EXPECT_EQ(LowEnergyScanner::State::kActiveScanning, this->scanner()->state());
  EXPECT_TRUE(this->scanner()->IsScanning());

  // StopScan() should terminate the scan session and the status should be
  // kStopped.
  EXPECT_TRUE(this->scanner()->StopScan());
  this->RunUntilIdle();

  EXPECT_EQ(LowEnergyScanner::ScanStatus::kStopped, this->last_scan_status());
  EXPECT_FALSE(this->test_device()->le_scan_state().enabled);
  EXPECT_TRUE(this->scanner()->IsIdle());
  EXPECT_FALSE(this->scanner()->IsScanning());
}

TYPED_TEST(LowEnergyScannerTest, StopScanWhileInitiating) {
  EXPECT_TRUE(this->scanner()->IsIdle());
  EXPECT_FALSE(this->scanner()->IsScanning());
  EXPECT_FALSE(this->test_device()->le_scan_state().enabled);

  EXPECT_TRUE(this->StartScan(true));
  EXPECT_EQ(LowEnergyScanner::State::kInitiating, this->scanner()->state());

  // Call StopScan(). This should cancel the HCI command sequence set up by
  // StartScan() so that the it never completes. The HCI_LE_Set_Scan_Parameters
  // command *may* get sent but the scan should never get enabled.
  EXPECT_TRUE(this->scanner()->StopScan());
  this->RunUntilIdle();

  EXPECT_EQ(LowEnergyScanner::ScanStatus::kStopped, this->last_scan_status());
  EXPECT_FALSE(this->test_device()->le_scan_state().enabled);
  EXPECT_TRUE(this->scanner()->IsIdle());
  EXPECT_FALSE(this->scanner()->IsScanning());
}

TYPED_TEST(LowEnergyScannerTest, ScanResponseTimeout) {
  constexpr pw::chrono::SystemClock::duration kHalfTimeout =
      kScanResponseTimeout / 2;

  std::unordered_set<DeviceAddress> results;
  this->set_peer_found_callback(
      [&](const std::unordered_set<uint16_t>& /*scan_ids*/,
          const LowEnergyScanResult& result) {
        results.insert(result.address());
      });

  // Add a peer that sends a scan response and one that doesn't.
  auto fake_peer = std::make_unique<FakePeer>(
      kRandomAddress1, this->dispatcher(), false, true);
  fake_peer->set_advertising_data(kPlainAdvDataBytes);
  fake_peer->set_scan_response(kPlainScanRspBytes);
  this->test_device()->AddPeer(std::move(fake_peer));

  fake_peer = std::make_unique<FakePeer>(
      kRandomAddress2, this->dispatcher(), true, false);
  fake_peer->set_advertising_data(kPlainAdvDataBytes);
  this->test_device()->AddPeer(std::move(fake_peer));

  EXPECT_TRUE(this->StartScan(true));
  this->RunUntilIdle();
  ASSERT_EQ(1u, results.size());
  EXPECT_EQ(1u, results.count(kRandomAddress1));

  // Advance the time but do not expire the timeout.
  this->RunFor(kHalfTimeout);
  ASSERT_EQ(1u, results.size());

  // Add another peer that doesn't send a scan response after the kHalfTimeout
  // delay. This is to test that a separate timeout is kept for every peer.
  fake_peer = std::make_unique<FakePeer>(
      kRandomAddress3, this->dispatcher(), true, false);
  fake_peer->set_advertising_data(kPlainAdvDataBytes);
  this->test_device()->AddPeer(std::move(fake_peer));

  // Expire the first timeout.
  this->RunFor(kHalfTimeout);
  ASSERT_EQ(2u, results.size());
  EXPECT_EQ(1u, results.count(kRandomAddress1));
  EXPECT_EQ(1u, results.count(kRandomAddress2));

  // Expire the second timeout.
  this->RunFor(kHalfTimeout);
  ASSERT_EQ(3u, results.size());
  EXPECT_EQ(1u, results.count(kRandomAddress1));
  EXPECT_EQ(1u, results.count(kRandomAddress2));
  EXPECT_EQ(1u, results.count(kRandomAddress3));
}

TYPED_TEST(LowEnergyScannerTest, ScanResponseAfterTimeout) {
  {
    auto peer = std::make_unique<FakePeer>(
        kPublicAddress1, this->dispatcher(), true, true, false);
    peer->set_advertising_data(kPlainAdvDataBytes);
    peer->set_scan_response(kPlainScanRspBytes);
    this->test_device()->AddPeer(std::move(peer));
  }
  auto peer = this->test_device()->FindPeer(kPublicAddress1);

  // The callback should get called on timeout waiting for a scan response
  bool peer_found_callback_called = false;
  std::unordered_map<DeviceAddress, std::unique_ptr<DynamicByteBuffer>> map;

  this->set_peer_found_callback(
      [&](const std::unordered_set<uint16_t>& /*scan_ids*/,
          const LowEnergyScanResult& result) {
        peer_found_callback_called = true;
        map[result.address()] =
            std::make_unique<DynamicByteBuffer>(result.data());
      });

  EXPECT_TRUE(this->StartScan(true));
  this->RunUntilIdle();

  this->test_device()->SendAdvertisingReport(*peer);
  this->RunFor(kPwScanResponseTimeout);
  ASSERT_TRUE(peer_found_callback_called);
  ASSERT_EQ(1u, map.count(peer->address()));
  EXPECT_EQ(kPlainAdvDataBytes.ToString(), map[peer->address()]->ToString());

  peer_found_callback_called = false;
  this->test_device()->SendScanResponseReport(*peer);
  this->RunUntilIdle();
  ASSERT_FALSE(peer_found_callback_called);
}

TYPED_TEST(LowEnergyScannerTest, ActiveScanResults) {
  // One of the 6 fake peers is scannable but never sends scan response
  // packets. That peer doesn't get reported until the end of the scan period.
  constexpr size_t kExpectedResultCount = 5u;

  this->AddFakePeers();

  std::map<DeviceAddress, LowEnergyScanResult> results;
  this->set_peer_found_callback(
      [&](const std::unordered_set<uint16_t>& /*scan_ids*/,
          const LowEnergyScanResult& result) {
        results[result.address()] = result;
      });

  // Perform an active scan.
  EXPECT_TRUE(this->StartScan(true, kPwScanPeriod));
  EXPECT_EQ(LowEnergyScanner::State::kInitiating, this->scanner()->state());

  this->RunUntilIdle();

  ASSERT_EQ(kExpectedResultCount, results.size());

  // Ending the scan period should notify Fake Peer #4.
  this->RunFor(kScanPeriod);
  EXPECT_EQ(LowEnergyScanner::ScanStatus::kComplete, this->last_scan_status());
  ASSERT_EQ(kExpectedResultCount + 1, results.size());

  // Verify the 6 results against the fake peers that were set up by
  // this->AddFakePeers(). Since the scan period ended naturally,
  // LowEnergyScanner should generate a peer found event for all pending reports
  // even if a scan response was not received for a scannable peer (see Fake
  // Peer 4, i.e. kRandomAddress3).

  // Result 0 (ADV_IND)
  {
    const auto& iter = results.find(kPublicAddress1);
    ASSERT_NE(iter, results.end());
    EXPECT_EQ(kAdvDataAndScanRsp, iter->second.data().ToString());
    EXPECT_EQ(kPublicAddress1, iter->second.address());
    EXPECT_TRUE(iter->second.connectable());
    results.erase(iter);
  }

  // Result 1 (ADV_SCAN_IND)
  {
    const auto& iter = results.find(kRandomAddress1);
    ASSERT_NE(iter, results.end());
    EXPECT_EQ(kAdvDataAndScanRsp, iter->second.data().ToString());
    EXPECT_EQ(kRandomAddress1, iter->second.address());
    EXPECT_FALSE(iter->second.connectable());
    results.erase(iter);
  }

  // Result 2 (ADV_IND), empty scan response
  {
    const auto& iter = results.find(kPublicAddress2);
    ASSERT_NE(iter, results.end());
    EXPECT_EQ(kPlainAdvData, iter->second.data().ToString());
    EXPECT_EQ(kPublicAddress2, iter->second.address());
    EXPECT_TRUE(iter->second.connectable());
    results.erase(iter);
  }

  // Result 3 (ADV_IND), empty advertising data w/ scan response
  {
    const auto& iter = results.find(kRandomAddress2);
    ASSERT_NE(iter, results.end());
    EXPECT_EQ(kAdvDataAndScanRsp, iter->second.data().ToString());
    EXPECT_EQ(kRandomAddress2, iter->second.address());
    EXPECT_TRUE(iter->second.connectable());
    results.erase(iter);
  }

  // Result 4 (ADV_IND), no scan response
  {
    const auto& iter = results.find(kRandomAddress3);
    ASSERT_NE(iter, results.end());
    EXPECT_EQ(kPlainAdvData, iter->second.data().ToString());
    EXPECT_EQ(kRandomAddress3, iter->second.address());
    EXPECT_TRUE(iter->second.connectable());
    results.erase(iter);
  }

  // Result 5 (ADV_NONCONN_IND)
  {
    const auto& iter = results.find(kRandomAddress4);
    ASSERT_NE(iter, results.end());
    EXPECT_EQ(kPlainAdvData, iter->second.data().ToString());
    EXPECT_EQ(kRandomAddress4, iter->second.address());
    EXPECT_FALSE(iter->second.connectable());
    results.erase(iter);
  }

  // No other reports are expected
  EXPECT_TRUE(results.empty());
}

TYPED_TEST(LowEnergyScannerTest, StopDuringActiveScan) {
  this->AddFakePeers();

  std::map<DeviceAddress, LowEnergyScanResult> results;
  this->set_peer_found_callback(
      [&results](const std::unordered_set<uint16_t>& /*scan_ids*/,
                 const LowEnergyScanResult& result) {
        results[result.address()] = result;
      });

  // Perform an active scan indefinitely. This means that the scan period will
  // never complete by itself.
  EXPECT_TRUE(this->StartScan(true));
  EXPECT_EQ(LowEnergyScanner::State::kInitiating, this->scanner()->state());
  this->RunUntilIdle();
  EXPECT_EQ(LowEnergyScanner::State::kActiveScanning, this->scanner()->state());

  // Run the loop until we've seen an event for the last peer that we
  // added. Fake Peer kRandomAddress3 is scannable but it never sends a scan
  // response so we expect that to remain in the scanner's pending reports list.
  this->RunUntilIdle();
  EXPECT_EQ(5u, results.size());
  EXPECT_EQ(results.find(kRandomAddress3), results.end());

  // Stop the scan. Since we are terminating the scan period early,
  // LowEnergyScanner should not send a report for the pending peer.
  EXPECT_TRUE(this->scanner()->StopScan());
  this->RunUntilIdle();
  EXPECT_TRUE(this->scanner()->IsIdle());

  EXPECT_EQ(5u, results.size());
  EXPECT_EQ(results.find(kRandomAddress3), results.end());
}

TYPED_TEST(LowEnergyScannerTest, PassiveScanResults) {
  constexpr size_t kExpectedResultCount = 6u;
  this->AddFakePeers();

  std::map<DeviceAddress, LowEnergyScanResult> results;
  this->set_peer_found_callback(
      [&](const std::unordered_set<uint16_t>& /*scan_ids*/,
          const LowEnergyScanResult& result) {
        results[result.address()] = result;
      });

  // Perform a passive scan.
  EXPECT_TRUE(this->StartScan(false));

  EXPECT_EQ(LowEnergyScanner::State::kInitiating, this->scanner()->state());

  this->RunUntilIdle();
  EXPECT_EQ(LowEnergyScanner::State::kPassiveScanning,
            this->scanner()->state());
  EXPECT_EQ(LowEnergyScanner::ScanStatus::kPassive, this->last_scan_status());
  ASSERT_EQ(kExpectedResultCount, results.size());

  // Verify the 6 results against the fake peers that were set up by
  // this->AddFakePeers(). All Scan Response PDUs should have been ignored.

  // Result 0
  {
    const auto& iter = results.find(kPublicAddress1);
    ASSERT_NE(iter, results.end());
    EXPECT_EQ(kPlainAdvData, iter->second.data().ToString());
    EXPECT_EQ(kPublicAddress1, iter->second.address());
    EXPECT_TRUE(iter->second.connectable());
    results.erase(iter);
  }

  // Result 1
  {
    const auto& iter = results.find(kRandomAddress1);
    ASSERT_NE(iter, results.end());
    EXPECT_EQ(kPlainAdvData, iter->second.data().ToString());
    EXPECT_EQ(kRandomAddress1, iter->second.address());
    EXPECT_FALSE(iter->second.connectable());
    results.erase(iter);
  }

  // Result 2
  {
    const auto& iter = results.find(kPublicAddress2);
    ASSERT_NE(iter, results.end());
    EXPECT_EQ(kPlainAdvData, iter->second.data().ToString());
    EXPECT_EQ(kPublicAddress2, iter->second.address());
    EXPECT_TRUE(iter->second.connectable());
    results.erase(iter);
  }

  // Result 3
  {
    const auto& iter = results.find(kRandomAddress2);
    ASSERT_NE(iter, results.end());
    EXPECT_EQ(kPlainAdvData, iter->second.data().ToString());
    EXPECT_EQ(kRandomAddress2, iter->second.address());
    EXPECT_TRUE(iter->second.connectable());
    results.erase(iter);
  }

  // Result 4
  {
    const auto& iter = results.find(kRandomAddress3);
    ASSERT_NE(iter, results.end());
    EXPECT_EQ(kPlainAdvData, iter->second.data().ToString());
    EXPECT_EQ(kRandomAddress3, iter->second.address());
    EXPECT_TRUE(iter->second.connectable());
    results.erase(iter);
  }

  // Result 5
  {
    const auto& iter = results.find(kRandomAddress4);
    ASSERT_NE(iter, results.end());
    EXPECT_EQ(kPlainAdvData, iter->second.data().ToString());
    EXPECT_EQ(kRandomAddress4, iter->second.address());
    EXPECT_FALSE(iter->second.connectable());
    results.erase(iter);
  }

  EXPECT_TRUE(results.empty());
}

TYPED_TEST(LowEnergyScannerTest, DirectedReport) {
  const auto& kPublicUnresolved = kPublicAddress1;
  const auto& kPublicResolved = kPublicAddress2;
  const auto& kRandomUnresolved = kRandomAddress1;
  const auto& kRandomResolved = kRandomAddress2;
  constexpr size_t kExpectedResultCount = 4u;

  // Unresolved public.
  auto fake_peer = std::make_unique<FakePeer>(
      kPublicUnresolved, this->dispatcher(), true, false);
  fake_peer->set_directed_advertising_enabled(true);
  this->test_device()->AddPeer(std::move(fake_peer));

  // Unresolved random.
  fake_peer = std::make_unique<FakePeer>(
      kRandomUnresolved, this->dispatcher(), true, false);
  fake_peer->set_directed_advertising_enabled(true);
  this->test_device()->AddPeer(std::move(fake_peer));

  // Resolved public.
  fake_peer = std::make_unique<FakePeer>(
      kPublicResolved, this->dispatcher(), true, false);
  fake_peer->set_address_resolved(true);
  fake_peer->set_directed_advertising_enabled(true);
  this->test_device()->AddPeer(std::move(fake_peer));

  // Resolved random.
  fake_peer = std::make_unique<FakePeer>(
      kRandomResolved, this->dispatcher(), true, false);
  fake_peer->set_address_resolved(true);
  fake_peer->set_directed_advertising_enabled(true);
  this->test_device()->AddPeer(std::move(fake_peer));

  std::unordered_map<DeviceAddress, LowEnergyScanResult> results;
  this->set_directed_adv_callback([&](const LowEnergyScanResult& result) {
    results[result.address()] = result;
  });

  EXPECT_TRUE(this->StartScan(true));
  EXPECT_EQ(LowEnergyScanner::State::kInitiating, this->scanner()->state());

  this->RunUntilIdle();

  ASSERT_EQ(LowEnergyScanner::ScanStatus::kActive, this->last_scan_status());
  ASSERT_EQ(kExpectedResultCount, results.size());

  ASSERT_TRUE(results.count(kPublicUnresolved));
  EXPECT_FALSE(results[kPublicUnresolved].resolved());

  ASSERT_TRUE(results.count(kRandomUnresolved));
  EXPECT_FALSE(results[kRandomUnresolved].resolved());

  ASSERT_TRUE(results.count(kPublicResolved));
  EXPECT_TRUE(results[kPublicResolved].resolved());

  ASSERT_TRUE(results.count(kRandomResolved));
  EXPECT_TRUE(results[kRandomResolved].resolved());
}

TYPED_TEST(LowEnergyScannerTest, AllowsRandomAddressChange) {
  EXPECT_TRUE(this->scanner()->AllowsRandomAddressChange());
  EXPECT_TRUE(this->StartScan(false));

  // Address change should not be allowed while the procedure is pending.
  EXPECT_TRUE(this->scanner()->IsInitiating());
  EXPECT_FALSE(this->scanner()->AllowsRandomAddressChange());

  this->RunUntilIdle();
  EXPECT_TRUE(this->scanner()->IsPassiveScanning());
  EXPECT_FALSE(this->scanner()->AllowsRandomAddressChange());
}

TYPED_TEST(LowEnergyScannerTest,
           AllowsRandomAddressChangeWhileRequestingLocalAddress) {
  // Make the local address delegate report its result asynchronously.
  this->fake_address_delegate()->set_async(true);
  EXPECT_TRUE(this->StartScan(false));

  // The scanner should be in the initiating state without initiating controller
  // procedures that would prevent a local address change.
  EXPECT_TRUE(this->scanner()->IsInitiating());
  EXPECT_TRUE(this->scanner()->AllowsRandomAddressChange());

  this->RunUntilIdle();
  EXPECT_TRUE(this->scanner()->IsPassiveScanning());
  EXPECT_FALSE(this->scanner()->AllowsRandomAddressChange());
}

TYPED_TEST(LowEnergyScannerTest, ScanUsingPublicAddress) {
  this->fake_address_delegate()->set_local_address(kPublicAddress1);
  EXPECT_TRUE(this->StartScan(false));
  this->RunUntilIdle();
  EXPECT_TRUE(this->scanner()->IsPassiveScanning());
  EXPECT_EQ(pw::bluetooth::emboss::LEOwnAddressType::PUBLIC,
            this->test_device()->le_scan_state().own_address_type);
}

TYPED_TEST(LowEnergyScannerTest, ScanUsingRandomAddress) {
  this->fake_address_delegate()->set_local_address(kRandomAddress1);
  // Public address would be used if privacy was disabled
  this->fake_address_delegate()->EnablePrivacy(true);
  EXPECT_TRUE(this->StartScan(false));
  this->RunUntilIdle();
  EXPECT_TRUE(this->scanner()->IsPassiveScanning());
  EXPECT_EQ(pw::bluetooth::emboss::LEOwnAddressType::RANDOM,
            this->test_device()->le_scan_state().own_address_type);
}

TYPED_TEST(LowEnergyScannerTest, StopScanWhileWaitingForLocalAddress) {
  this->fake_address_delegate()->set_async(true);
  EXPECT_TRUE(this->StartScan(false));

  // Should be waiting for the random address.
  EXPECT_TRUE(this->scanner()->IsInitiating());
  EXPECT_TRUE(this->scanner()->AllowsRandomAddressChange());

  EXPECT_TRUE(this->scanner()->StopScan());
  this->RunUntilIdle();

  // Should end up not scanning.
  EXPECT_TRUE(this->scanner()->IsIdle());
  EXPECT_FALSE(this->test_device()->le_scan_state().enabled);
}

TYPED_TEST(LowEnergyScannerTest, CallbackStopsScanning) {
  auto fake_peer = std::make_unique<FakePeer>(
      kRandomAddress1, this->dispatcher(), true, false);
  fake_peer->set_advertising_data(kPlainAdvDataBytes);
  this->test_device()->AddPeer(std::move(fake_peer));

  fake_peer = std::make_unique<FakePeer>(
      kRandomAddress2, this->dispatcher(), true, false);
  fake_peer->set_advertising_data(kPlainAdvDataBytes);
  this->test_device()->AddPeer(std::move(fake_peer));

  // We should be able to stop scanning in the callback and not crash. Note: if
  // crashing, it will likely be due to a use-after-free type bug. Such a bug
  // may or may not manifest itself in a non-asan build.
  this->set_peer_found_callback(
      [&](const std::unordered_set<uint16_t>& /*scan_ids*/,
          const LowEnergyScanResult&) { this->scanner()->StopScan(); });

  EXPECT_TRUE(this->StartScan(true, kPwScanPeriod));
  this->RunFor(kScanPeriod);
}

TYPED_TEST(LowEnergyScannerTest, FilterPeers) {
  auto fake_peer = std::make_unique<FakePeer>(
      kRandomAddress1, this->dispatcher(), true, true);
  fake_peer->set_advertising_data(kPlainAdvDataBytes);
  fake_peer->set_scan_response(kPlainScanRspBytes);
  this->test_device()->AddPeer(std::move(fake_peer));

  fake_peer = std::make_unique<FakePeer>(
      kRandomAddress2, this->dispatcher(), false, false);
  fake_peer->set_advertising_data(kPlainAdvDataBytes);
  this->test_device()->AddPeer(std::move(fake_peer));

  DiscoveryFilter filter;
  filter.set_connectable(true);
  this->scanner()->SetPacketFilters(0, {filter});

  std::unordered_set<DeviceAddress> results;
  this->set_peer_found_callback(
      [&](const std::unordered_set<uint16_t>& /*scan_ids*/,
          const LowEnergyScanResult& result) {
        results.insert(result.address());
      });

  EXPECT_TRUE(this->StartScan(true));
  this->RunUntilIdle();
  ASSERT_EQ(1u, results.size());
  EXPECT_EQ(1u, results.count(kRandomAddress1));
}

TYPED_TEST(LowEnergyScannerTest, CachedScanResultsAreFiltered) {
  EXPECT_TRUE(this->StartScan(true));

  auto fake_peer = std::make_unique<FakePeer>(
      kRandomAddress1, this->dispatcher(), true, true);
  fake_peer->set_advertising_data(kPlainAdvDataBytes);
  fake_peer->set_scan_response(kPlainScanRspBytes);
  this->test_device()->AddPeer(std::move(fake_peer));

  fake_peer = std::make_unique<FakePeer>(
      kRandomAddress2, this->dispatcher(), false, false);
  fake_peer->set_advertising_data(kPlainAdvDataBytes);
  this->test_device()->AddPeer(std::move(fake_peer));
  this->RunUntilIdle();

  std::unordered_set<DeviceAddress> results;
  this->set_peer_found_callback(
      [&](const std::unordered_set<uint16_t>& /*scan_ids*/,
          const LowEnergyScanResult& result) {
        results.insert(result.address());
      });

  DiscoveryFilter filter;
  filter.set_connectable(true);
  this->scanner()->SetPacketFilters(0, {filter});
  this->scanner()->NotifyCachedPeers(0);

  this->RunUntilIdle();
  ASSERT_EQ(1u, results.size());
  EXPECT_EQ(1u, results.count(kRandomAddress1));
}

TYPED_TEST(LowEnergyScannerTest, DISABLED_NewFilterWhileOffloadingEnabled) {
  this->EnableOffloading();

  EXPECT_TRUE(this->StartScan(true));

  auto fake_peer = std::make_unique<FakePeer>(
      kRandomAddress1, this->dispatcher(), true, true);
  fake_peer->set_advertising_data(kPlainAdvDataBytes);
  fake_peer->set_scan_response(kPlainScanRspBytes);
  this->test_device()->AddPeer(std::move(fake_peer));

  fake_peer = std::make_unique<FakePeer>(
      kRandomAddress2, this->dispatcher(), false, false);
  fake_peer->set_advertising_data(kPlainAdvDataBytes);
  this->test_device()->AddPeer(std::move(fake_peer));

  std::unordered_set<DeviceAddress> results;
  this->set_peer_found_callback(
      [&](const std::unordered_set<uint16_t>& /*scan_ids*/,
          const LowEnergyScanResult& result) {
        results.insert(result.address());
      });
  this->RunUntilIdle();

  ASSERT_EQ(2u, results.size());
  EXPECT_EQ(1u, results.count(kRandomAddress1));
  EXPECT_EQ(1u, results.count(kRandomAddress2));

  results.clear();
  this->scanner()->SetPacketFilters(0, {});
  this->scanner()->NotifyCachedPeers(0);

  this->RunUntilIdle();
  ASSERT_EQ(0u, results.size());
}

TEST(LowEnergyScanResultTest, AssignmentOperator) {
  constexpr int8_t kRSSI = -18;
  constexpr uint8_t kAdvertisingSid = 0x0d;
  constexpr uint16_t kPeriodicAdvertisingInterval = 0x2468;
  LowEnergyScanResult scan_result_1(
      kPublicAddress1, /*resolved=*/true, /*connectable=*/true);
  scan_result_1.set_rssi(kRSSI);
  scan_result_1.set_advertising_sid(kAdvertisingSid);
  scan_result_1.set_periodic_advertising_interval(kPeriodicAdvertisingInterval);

  LowEnergyScanResult scan_result_2 = scan_result_1;
  EXPECT_EQ(scan_result_2.address(), kPublicAddress1);
  EXPECT_EQ(scan_result_2.resolved(), true);
  EXPECT_EQ(scan_result_2.connectable(), true);
  EXPECT_EQ(scan_result_2.rssi(), kRSSI);
  EXPECT_EQ(scan_result_2.advertising_sid(), kAdvertisingSid);
  EXPECT_EQ(scan_result_2.periodic_advertising_interval(),
            kPeriodicAdvertisingInterval);
}

}  // namespace bt::hci
