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

#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_advertising_manager.h"

#include <pw_assert/check.h>

#include <map>

#include "pw_async/dispatcher.h"
#include "pw_bluetooth_sapphire/internal/host/common/advertising_data.h"
#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/hci/connection.h"
#include "pw_bluetooth_sapphire/internal/host/hci/fake_local_address_delegate.h"
#include "pw_bluetooth_sapphire/internal/host/hci/fake_low_energy_connection.h"
#include "pw_bluetooth_sapphire/internal/host/testing/controller_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_controller.h"
#include "pw_bluetooth_sapphire/internal/host/transport/control_packets.h"
#include "pw_bluetooth_sapphire/internal/host/transport/error.h"

namespace bt {
using testing::FakeController;

namespace gap {
namespace {
namespace pwemb = pw::bluetooth::emboss;
using TestingBase = bt::testing::FakeDispatcherControllerTest<FakeController>;

constexpr size_t kDefaultMaxAdSize = 23;
constexpr size_t kDefaultFakeAdSize = 20;
constexpr AdvertisingInterval kTestInterval = AdvertisingInterval::FAST1;

const DeviceAddress kPublicAddress(DeviceAddress::Type::kLEPublic,
                                   {0x01, 0, 0, 0, 0, 0});
const DeviceAddress kRandomAddress(DeviceAddress::Type::kLERandom,
                                   {0x55, 0x44, 0x33, 0x22, 0x11, 0x00});

void NopConnectCallback(AdvertisementId, std::unique_ptr<hci::Connection>) {}

struct AdvertisementStatus {
  DeviceAddress address;
  AdvertisingData data;
  AdvertisingData scan_rsp;
  bool anonymous;
  uint32_t interval_min;
  uint32_t interval_max;
  int8_t tx_power;
  bool extended_pdu;
  hci::LowEnergyAdvertiser::ConnectionCallback connect_cb;
};

// LowEnergyAdvertiser for testing purposes:
//  - Reports mas_ad_size supported
//  - Actually just accepts all ads and stores them in ad_store
class FakeLowEnergyAdvertiser final : public hci::LowEnergyAdvertiser {
 public:
  FakeLowEnergyAdvertiser(
      const hci::Transport::WeakPtr& hci,
      pw::async::Dispatcher& dispatcher,
      std::unordered_map<AdvertisementId, AdvertisementStatus>* ad_store)
      : hci::LowEnergyAdvertiser(hci, dispatcher, kDefaultMaxAdSize),
        ads_(ad_store),
        hci_(hci),
        dispatcher_(dispatcher) {
    PW_CHECK(ads_);
  }

  ~FakeLowEnergyAdvertiser() override = default;

  size_t MaxAdvertisements() const override { return 1; }

  bool AllowsRandomAddressChange() const override { return true; }

  bool IsAdvertising() const override { return !ads_->empty(); }

  bool IsExtendedAdvertiser() const override { return extended_; }

  void set_extended(bool extended) { extended_ = extended; }

  void StartAdvertising(
      const DeviceAddress& address,
      const AdvertisingData& data,
      const AdvertisingData& scan_rsp,
      const AdvertisingOptions& options,
      ConnectionCallback connect_callback,
      hci::ResultFunction<AdvertisementId> result_callback) override {
    if (pending_error_.is_error()) {
      result_callback(fit::error(pending_error_.error_value()));
      pending_error_ = fit::ok();
      return;
    }

    fit::result<HostError> result =
        CanStartAdvertising(address, data, scan_rsp, options, connect_callback);
    if (result.is_error()) {
      result_callback(fit::error(result.error_value()));
      return;
    }

    AdvertisementStatus new_status;
    data.Copy(&new_status.data);
    scan_rsp.Copy(&new_status.scan_rsp);
    new_status.address = address;
    new_status.connect_cb = std::move(connect_callback);
    new_status.interval_min = options.interval.min();
    new_status.interval_max = options.interval.max();
    new_status.tx_power = options.max_tx_power;
    new_status.anonymous = options.anonymous;
    new_status.extended_pdu = options.extended_pdu;
    AdvertisementId adv_id(next_id_++);
    ads_->emplace(adv_id, std::move(new_status));
    result_callback(fit::ok(adv_id));
  }

  void StopAdvertising(
      fit::function<void(hci::Result<>)> result_cb = nullptr) override {
    StopAdvertisingInternal(std::move(result_cb));
  }
  void StopAdvertising(AdvertisementId adv_id,
                       fit::function<void(hci::Result<>)>) override {
    ads_->erase(adv_id);
  }

  void OnIncomingConnection(hci_spec::ConnectionHandle handle,
                            pwemb::ConnectionRole role,
                            const DeviceAddress& peer_address,
                            const hci_spec::LEConnectionParameters&) override {
    // Right now, we call the first callback, because we can't call any other
    // ones.
    // TODO(jamuraa): make this send it to the correct callback once we can
    // determine which one that is.
    const auto& cb = ads_->begin()->second.connect_cb;
    if (cb) {
      cb(ads_->begin()->first,
         std::make_unique<hci::testing::FakeLowEnergyConnection>(
             handle,
             ads_->begin()->second.address,
             peer_address,
             role,
             hci_,
             dispatcher_));
    }
  }

  // Sets this faker up to send an error back from the next StartAdvertising
  // call. Set to success to disable a previously called error.
  void ErrorOnNext(hci::Result<> error_status) {
    pending_error_ = error_status;
  }

 private:
  hci::CommandPacket BuildEnablePacket(
      AdvertisementId,
      pw::bluetooth::emboss::GenericEnableParam) const override {
    return hci::CommandPacket::New<
        pwemb::LESetExtendedAdvertisingEnableDataWriter>(
        hci_spec::kLESetExtendedAdvertisingEnable);
  }

  std::optional<hci::LowEnergyAdvertiser::SetAdvertisingParams>
  BuildSetAdvertisingParams(const DeviceAddress&,
                            const AdvertisingEventProperties&,
                            const AdvertisingOptions&,
                            pwemb::LEOwnAddressType,
                            const hci::AdvertisingIntervalRange&) override {
    return std::nullopt;
  }

  std::optional<hci::CommandPacket> BuildSetAdvertisingRandomAddr(
      AdvertisementId) const override {
    return std::nullopt;
  }

  std::vector<hci::CommandPacket> BuildSetAdvertisingData(
      AdvertisementId, const AdvertisingData&, AdvFlags) const override {
    hci::CommandPacket packet =
        hci::CommandPacket::New<pwemb::LESetAdvertisingDataCommandWriter>(
            hci_spec::kLESetAdvertisingData);

    std::vector<hci::CommandPacket> packets;
    packets.push_back(std::move(packet));
    return packets;
  }

  hci::CommandPacket BuildUnsetAdvertisingData(AdvertisementId) const override {
    return hci::CommandPacket::New<pwemb::LESetAdvertisingDataCommandWriter>(
        hci_spec::kLESetAdvertisingData);
  }

  std::vector<hci::CommandPacket> BuildSetScanResponse(
      AdvertisementId, const AdvertisingData&) const override {
    hci::CommandPacket packet =
        hci::CommandPacket::New<pwemb::LESetScanResponseDataCommandWriter>(
            hci_spec::kLESetScanResponseData);

    std::vector<hci::CommandPacket> packets;
    packets.push_back(std::move(packet));
    return packets;
  }

  hci::CommandPacket BuildUnsetScanResponse(AdvertisementId) const override {
    return hci::CommandPacket::New<pwemb::LESetScanResponseDataCommandWriter>(
        hci_spec::kLESetScanResponseData);
  }

  std::optional<hci::CommandPacket> BuildRemoveAdvertisingSet(
      AdvertisementId) const override {
    return hci::CommandPacket::New<pwemb::LERemoveAdvertisingSetCommandWriter>(
        hci_spec::kLERemoveAdvertisingSet);
  }

  std::unordered_map<AdvertisementId, AdvertisementStatus>* ads_;
  bool extended_ = false;
  hci::Result<> pending_error_ = fit::ok();
  hci::Transport::WeakPtr hci_;
  pw::async::Dispatcher& dispatcher_;
  AdvertisementId::value_t next_id_{1};

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(FakeLowEnergyAdvertiser);
};

class LowEnergyAdvertisingManagerTest : public TestingBase {
 public:
  LowEnergyAdvertisingManagerTest() = default;
  ~LowEnergyAdvertisingManagerTest() override = default;

 protected:
  void SetUp() override {
    TestingBase::SetUp();
    InitializeACLDataChannel();

    fake_address_delegate_.EnablePrivacy(true);
    fake_address_delegate_.set_identity_address(kPublicAddress);
    fake_address_delegate_.set_local_address(kRandomAddress);
    MakeFakeAdvertiser();
    MakeAdvertisingManager();
  }

  void TearDown() override {
    adv_mgr_ = nullptr;
    advertiser_ = nullptr;
    TestingBase::TearDown();
  }

  // Makes some fake advertising data of a specific |packed_size|
  AdvertisingData CreateFakeAdvertisingData(
      size_t packed_size = kDefaultFakeAdSize) {
    AdvertisingData result;
    StaticByteBuffer buffer(
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08);
    size_t bytes_left = packed_size;
    while (bytes_left > 0) {
      // Each field to take 10 bytes total, unless the next header (4 bytes)
      // won't fit. In which case we add enough bytes to finish up.
      size_t data_bytes = bytes_left < 14 ? (bytes_left - 4) : 6;
      EXPECT_TRUE(result.SetManufacturerData(0xb000 + bytes_left,
                                             buffer.view(0, data_bytes)));
      bytes_left = packed_size - result.CalculateBlockSize();
    }
    return result;
  }

  LowEnergyAdvertisingManager::AdvertisingStatusCallback GetErrorCallback() {
    return [this](AdvertisementInstance instance, hci::Result<> status) {
      EXPECT_EQ(kInvalidAdvertisementId, instance.id());
      EXPECT_TRUE(status.is_error());
      last_status_ = status;
    };
  }

  LowEnergyAdvertisingManager::AdvertisingStatusCallback GetSuccessCallback() {
    return [this](AdvertisementInstance instance, hci::Result<> status) {
      EXPECT_NE(kInvalidAdvertisementId, instance.id());
      EXPECT_EQ(fit::ok(), status);
      last_instance_ = std::move(instance);
      last_status_ = status;
    };
  }

  void MakeFakeAdvertiser() {
    advertiser_ = std::make_unique<FakeLowEnergyAdvertiser>(
        transport()->GetWeakPtr(), dispatcher(), &ad_store_);
  }

  void MakeAdvertisingManager() {
    adv_mgr_ = std::make_unique<LowEnergyAdvertisingManager>(
        advertiser(), &fake_address_delegate_);
  }

  LowEnergyAdvertisingManager* adv_mgr() const { return adv_mgr_.get(); }
  const std::unordered_map<AdvertisementId, AdvertisementStatus>& ad_store() {
    return ad_store_;
  }
  AdvertisementId last_ad_id() const { return last_instance_.id(); }

  // Returns the currently active advertising state. This is useful for tests
  // that want to verify advertising parameters when there is a single known
  // advertisement. Returns nullptr if the number of advertisements are not
  // equal to one.
  const AdvertisementStatus* current_adv() const {
    if (ad_store_.size() != 1u) {
      return nullptr;
    }
    return &ad_store_.begin()->second;
  }

  void DestroyAdvertisementInstance() {
    last_instance_ = AdvertisementInstance();
  }

  void TestIntervalRange(
      fit::function<void(uint32_t, uint32_t)> set_interval_func,
      AdvertisingInterval interval_enum,
      bool connectable) {
    const uint32_t kLimitMin = advertiser()->IsExtendedAdvertiser()
                                   ? hci_spec::kLEExtendedAdvertisingIntervalMin
                                   : hci_spec::kLEAdvertisingIntervalMin;
    const uint32_t kLimitMax = advertiser()->IsExtendedAdvertiser()
                                   ? hci_spec::kLEExtendedAdvertisingIntervalMax
                                   : hci_spec::kLEAdvertisingIntervalMax;

    auto start_adv = [&](AdvertisingInterval interval) {
      adv_mgr()->StartAdvertising(AdvertisingData(),
                                  AdvertisingData(),
                                  connectable ? NopConnectCallback : nullptr,
                                  interval,
                                  /*extended_pdu=*/false,
                                  /*anonymous=*/false,
                                  /*include_tx_power_level=*/false,
                                  /*address_type=*/std::nullopt,
                                  GetSuccessCallback());
      RunUntilIdle();
    };

    // Valid values
    set_interval_func(kLimitMin + 1, kLimitMax - 1);
    start_adv(interval_enum);
    ASSERT_TRUE(current_adv());
    EXPECT_EQ(kLimitMin + 1, current_adv()->interval_min);
    EXPECT_EQ(kLimitMax - 1, current_adv()->interval_max);
    DestroyAdvertisementInstance();

    // Invalid values (too large)
    set_interval_func(kLimitMin, kLimitMax + 1);
    start_adv(interval_enum);
    ASSERT_TRUE(current_adv());
    EXPECT_EQ(kLimitMin + 1, current_adv()->interval_min);
    EXPECT_EQ(kLimitMax - 1, current_adv()->interval_max);
    DestroyAdvertisementInstance();

    // min == 0 (ignored)
    set_interval_func(0, kLimitMax - 1);
    start_adv(interval_enum);
    ASSERT_TRUE(current_adv());
    EXPECT_EQ(kLimitMin + 1, current_adv()->interval_min);
    EXPECT_EQ(kLimitMax - 1, current_adv()->interval_max);
    DestroyAdvertisementInstance();

    // max == 0 (ignored)
    set_interval_func(kLimitMin + 1, 0);
    start_adv(interval_enum);
    ASSERT_TRUE(current_adv());
    EXPECT_EQ(kLimitMin + 1, current_adv()->interval_min);
    EXPECT_EQ(kLimitMax - 1, current_adv()->interval_max);
    DestroyAdvertisementInstance();

    // min < limit_min (ignored)
    set_interval_func(kLimitMin - 1, kLimitMax - 1);
    start_adv(interval_enum);
    ASSERT_TRUE(current_adv());
    EXPECT_EQ(kLimitMin + 1, current_adv()->interval_min);
    EXPECT_EQ(kLimitMax - 1, current_adv()->interval_max);
    DestroyAdvertisementInstance();

    if (advertiser()->IsExtendedAdvertiser()) {
      // min == max is valid
      set_interval_func(0x0100, 0x0100);
      start_adv(interval_enum);
      ASSERT_TRUE(current_adv());
      EXPECT_EQ(0x0100u, current_adv()->interval_min);
      EXPECT_EQ(0x0100u, current_adv()->interval_max);
      DestroyAdvertisementInstance();

      // min > max is always invalid
      set_interval_func(0x0101, 0x0100);
      start_adv(interval_enum);
      ASSERT_TRUE(current_adv());
      EXPECT_EQ(0x0100u, current_adv()->interval_min);
      EXPECT_EQ(0x0100u, current_adv()->interval_max);
      DestroyAdvertisementInstance();
    }
  }

  void TestAdvertisingTxPower(fit::function<void(int8_t)> set_tx_power_func,
                              AdvertisingInterval interval_enum,
                              bool connectable,
                              int8_t expected_default_power) {
    auto start_adv = [&](AdvertisingInterval interval) {
      adv_mgr()->StartAdvertising(AdvertisingData(),
                                  AdvertisingData(),
                                  connectable ? NopConnectCallback : nullptr,
                                  interval,
                                  /*extended_pdu=*/false,
                                  /*anonymous=*/false,
                                  /*include_tx_power_level=*/false,
                                  /*address_type=*/std::nullopt,
                                  GetSuccessCallback());
      RunUntilIdle();
    };

    // Valid values
    set_tx_power_func(hci_spec::kLEAdvertisingTxPowerMin);
    start_adv(interval_enum);
    ASSERT_TRUE(current_adv());
    EXPECT_EQ(hci_spec::kLEAdvertisingTxPowerMin, current_adv()->tx_power);
    DestroyAdvertisementInstance();

    set_tx_power_func(0);
    start_adv(interval_enum);
    ASSERT_TRUE(current_adv());
    EXPECT_EQ(0, current_adv()->tx_power);
    DestroyAdvertisementInstance();

    set_tx_power_func(hci_spec::kLEAdvertisingTxPowerMax);
    start_adv(interval_enum);
    ASSERT_TRUE(current_adv());
    EXPECT_EQ(hci_spec::kLEAdvertisingTxPowerMax, current_adv()->tx_power);
    DestroyAdvertisementInstance();

    // No preference should be accepted
    set_tx_power_func(hci_spec::kLEExtendedAdvertisingTxPowerNoPreference);
    start_adv(interval_enum);
    ASSERT_TRUE(current_adv());
    EXPECT_EQ(hci_spec::kLEExtendedAdvertisingTxPowerNoPreference,
              current_adv()->tx_power);
    DestroyAdvertisementInstance();

    // Reset to a known valid value
    set_tx_power_func(expected_default_power);

    // Invalid values (too small)
    set_tx_power_func(hci_spec::kLEAdvertisingTxPowerMin - 1);
    start_adv(interval_enum);
    ASSERT_TRUE(current_adv());
    EXPECT_EQ(expected_default_power, current_adv()->tx_power);
    DestroyAdvertisementInstance();

    // Invalid values (too large)
    set_tx_power_func(hci_spec::kLEAdvertisingTxPowerMax + 1);
    start_adv(interval_enum);
    ASSERT_TRUE(current_adv());
    EXPECT_EQ(expected_default_power, current_adv()->tx_power);
    DestroyAdvertisementInstance();
  }

  // Returns and clears the last callback status. This resets the state to
  // detect another callback.
  std::optional<hci::Result<>> last_status() { return last_status_; }

  FakeLowEnergyAdvertiser* advertiser() const { return advertiser_.get(); }

 private:
  hci::FakeLocalAddressDelegate fake_address_delegate_{dispatcher()};

  // TODO(armansito): The address mapping is currently broken since the
  // gap::LEAM always assigns the controller random address. Make this track
  // each instance by instance ID instead once the layering issues have been
  // fixed.
  std::unordered_map<AdvertisementId, AdvertisementStatus> ad_store_;
  AdvertisementInstance last_instance_;
  std::optional<hci::Result<>> last_status_;
  std::unique_ptr<FakeLowEnergyAdvertiser> advertiser_;
  std::unique_ptr<LowEnergyAdvertisingManager> adv_mgr_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(LowEnergyAdvertisingManagerTest);
};

// Tests:
//  - When the advertiser succeeds, the callback is called with the success
TEST_F(LowEnergyAdvertisingManagerTest, Success) {
  EXPECT_FALSE(adv_mgr()->advertising());
  adv_mgr()->StartAdvertising(CreateFakeAdvertisingData(),
                              AdvertisingData(),
                              /*connect_callback=*/nullptr,
                              kTestInterval,
                              /*extended_pdu=*/false,
                              /*anonymous=*/false,
                              /*include_tx_power_level=*/false,
                              /*address_type=*/std::nullopt,
                              GetSuccessCallback());

  RunUntilIdle();

  EXPECT_TRUE(last_status());
  ASSERT_EQ(1u, ad_store().size());
  EXPECT_TRUE(adv_mgr()->advertising());

  // Verify that the advertiser uses the requested local address.
  EXPECT_EQ(kRandomAddress, ad_store().begin()->second.address);
}

TEST_F(LowEnergyAdvertisingManagerTest, DataSize) {
  adv_mgr()->StartAdvertising(CreateFakeAdvertisingData(),
                              AdvertisingData(),
                              /*connect_callback=*/nullptr,
                              kTestInterval,
                              /*extended_pdu=*/true,
                              /*anonymous=*/false,
                              /*include_tx_power_level=*/false,
                              /*address_type=*/std::nullopt,
                              GetSuccessCallback());

  RunUntilIdle();

  EXPECT_TRUE(last_status());
  EXPECT_EQ(1u, ad_store().size());

  adv_mgr()->StartAdvertising(CreateFakeAdvertisingData(kDefaultMaxAdSize + 1),
                              AdvertisingData(),
                              /*connect_callback=*/nullptr,
                              kTestInterval,
                              /*extended_pdu=*/true,
                              /*anonymous=*/false,
                              /*include_tx_power_level=*/false,
                              /*address_type=*/std::nullopt,
                              GetErrorCallback());

  RunUntilIdle();

  EXPECT_TRUE(last_status());
  EXPECT_EQ(1u, ad_store().size());
}

// TODO: https://fxbug.dev/42083437 - Revise this test to use multiple
// advertising instances when multi-advertising is supported.
//  - Stopping one that is registered stops it in the advertiser
//    (and stops the right address)
//  - Stopping an advertisement that isn't registered returns false
TEST_F(LowEnergyAdvertisingManagerTest, RegisterUnregister) {
  adv_mgr()->StartAdvertising(CreateFakeAdvertisingData(),
                              AdvertisingData(),
                              /*connect_callback=*/nullptr,
                              kTestInterval,
                              /*extended_pdu=*/false,
                              /*anonymous=*/false,
                              /*include_tx_power_level=*/false,
                              /*address_type=*/std::nullopt,
                              GetSuccessCallback());

  RunUntilIdle();

  EXPECT_TRUE(last_status());
  EXPECT_EQ(1u, ad_store().size());
  EXPECT_TRUE(adv_mgr()->advertising());

  DestroyAdvertisementInstance();
  EXPECT_TRUE(ad_store().empty());
  EXPECT_FALSE(adv_mgr()->advertising());
}

//  - When the advertiser returns an error, we return an error
TEST_F(LowEnergyAdvertisingManagerTest, AdvertiserError) {
  advertiser()->ErrorOnNext(
      ToResult(pwemb::StatusCode::INVALID_HCI_COMMAND_PARAMETERS));

  EXPECT_FALSE(adv_mgr()->advertising());
  adv_mgr()->StartAdvertising(CreateFakeAdvertisingData(),
                              AdvertisingData(),
                              /*connect_callback=*/nullptr,
                              kTestInterval,
                              /*extended_pdu=*/false,
                              /*anonymous=*/false,
                              /*include_tx_power_level=*/false,
                              /*address_type=*/std::nullopt,
                              GetErrorCallback());
  RunUntilIdle();

  EXPECT_TRUE(last_status());
  EXPECT_FALSE(adv_mgr()->advertising());
}

//  - It calls the connectable callback correctly when connected to
TEST_F(LowEnergyAdvertisingManagerTest, ConnectCallback) {
  std::unique_ptr<hci::LowEnergyConnection> link;
  AdvertisementId advertised_id = kInvalidAdvertisementId;

  auto connect_cb = [&](AdvertisementId connected_id,
                        std::unique_ptr<hci::LowEnergyConnection> cb_link) {
    link = std::move(cb_link);
    EXPECT_EQ(advertised_id, connected_id);
  };
  adv_mgr()->StartAdvertising(CreateFakeAdvertisingData(),
                              AdvertisingData(),
                              connect_cb,
                              kTestInterval,
                              /*extended_pdu=*/false,
                              /*anonymous=*/false,
                              /*include_tx_power_level=*/false,
                              /*address_type=*/std::nullopt,
                              GetSuccessCallback());

  RunUntilIdle();

  EXPECT_TRUE(last_status());
  advertised_id = last_ad_id();

  DeviceAddress peer_address(DeviceAddress::Type::kLEPublic,
                             {3, 2, 1, 1, 2, 3});
  advertiser()->OnIncomingConnection(1,
                                     pwemb::ConnectionRole::PERIPHERAL,
                                     peer_address,
                                     hci_spec::LEConnectionParameters());
  RunUntilIdle();
  ASSERT_TRUE(link);

  // Make sure that the link has the correct local and peer addresses
  // assigned.
  EXPECT_EQ(kRandomAddress, link->local_address());
  EXPECT_EQ(peer_address, link->peer_address());
}

//  - Error: Connectable and Anonymous at the same time
TEST_F(LowEnergyAdvertisingManagerTest, ConnectAdvertiseError) {
  auto connect_cb = [](AdvertisementId,
                       std::unique_ptr<hci::LowEnergyConnection>) {};

  adv_mgr()->StartAdvertising(CreateFakeAdvertisingData(),
                              AdvertisingData(),
                              connect_cb,
                              kTestInterval,
                              /*extended_pdu=*/false,
                              /*anonymous=*/true,
                              /*include_tx_power_level=*/false,
                              /*address_type=*/std::nullopt,
                              GetErrorCallback());

  EXPECT_TRUE(last_status());
}

// Passes the values for the data on. (anonymous, data, scan_rsp)
TEST_F(LowEnergyAdvertisingManagerTest, SendsCorrectData) {
  adv_mgr()->StartAdvertising(CreateFakeAdvertisingData(),
                              CreateFakeAdvertisingData(/*packed_size=*/21),
                              /*connect_callback=*/nullptr,
                              kTestInterval,
                              /*extended_pdu=*/false,
                              /*anonymous=*/false,
                              /*include_tx_power_level=*/false,
                              /*address_type=*/std::nullopt,
                              GetSuccessCallback());

  RunUntilIdle();

  EXPECT_TRUE(last_status());
  EXPECT_EQ(1u, ad_store().size());

  auto ad_status = &ad_store().begin()->second;

  AdvertisingData expected_ad = CreateFakeAdvertisingData();
  AdvertisingData expected_scan_rsp =
      CreateFakeAdvertisingData(/*packed_size=*/21);
  EXPECT_EQ(expected_ad, ad_status->data);
  EXPECT_EQ(expected_scan_rsp, ad_status->scan_rsp);
  EXPECT_EQ(false, ad_status->anonymous);
  EXPECT_EQ(nullptr, ad_status->connect_cb);
}

// Test that the AdvertisingInterval values map to the spec defined constants
// (NOTE: this might change in the future in favor of a more advanced policy
// for managing the intervals; for now they get mapped to recommended values
// from Vol 3, Part C, Appendix A).
TEST_F(LowEnergyAdvertisingManagerTest, ConnectableAdvertisingIntervals) {
  adv_mgr()->StartAdvertising(CreateFakeAdvertisingData(),
                              CreateFakeAdvertisingData(/*packed_size=*/21),
                              NopConnectCallback,
                              AdvertisingInterval::FAST1,
                              /*extended_pdu=*/false,
                              /*anonymous=*/false,
                              /*include_tx_power_level=*/false,
                              /*address_type=*/std::nullopt,
                              GetSuccessCallback());
  RunUntilIdle();
  ASSERT_TRUE(last_status());
  ASSERT_TRUE(current_adv());
  EXPECT_EQ(kLEAdvertisingFastIntervalMin1, current_adv()->interval_min);
  EXPECT_EQ(kLEAdvertisingFastIntervalMax1, current_adv()->interval_max);
  DestroyAdvertisementInstance();

  adv_mgr()->StartAdvertising(CreateFakeAdvertisingData(),
                              CreateFakeAdvertisingData(/*packed_size=*/21),
                              NopConnectCallback,
                              AdvertisingInterval::FAST2,
                              /*extended_pdu=*/false,
                              /*anonymous=*/false,
                              /*include_tx_power_level=*/false,
                              /*address_type=*/std::nullopt,
                              GetSuccessCallback());
  RunUntilIdle();
  ASSERT_TRUE(last_status());
  ASSERT_TRUE(current_adv());
  EXPECT_EQ(kLEAdvertisingFastIntervalMin2, current_adv()->interval_min);
  EXPECT_EQ(kLEAdvertisingFastIntervalMax2, current_adv()->interval_max);
  DestroyAdvertisementInstance();

  adv_mgr()->StartAdvertising(CreateFakeAdvertisingData(),
                              CreateFakeAdvertisingData(/*packed_size=*/21),
                              NopConnectCallback,
                              AdvertisingInterval::SLOW,
                              /*extended_pdu=*/false,
                              /*anonymous=*/false,
                              /*include_tx_power_level=*/false,
                              /*address_type=*/std::nullopt,
                              GetSuccessCallback());
  RunUntilIdle();
  ASSERT_TRUE(last_status());
  ASSERT_TRUE(current_adv());
  EXPECT_EQ(kLEAdvertisingSlowIntervalMin, current_adv()->interval_min);
  EXPECT_EQ(kLEAdvertisingSlowIntervalMax, current_adv()->interval_max);
  DestroyAdvertisementInstance();
}

TEST_F(LowEnergyAdvertisingManagerTest, NonConnectableAdvertisingIntervals) {
  AdvertisingData fake_ad = CreateFakeAdvertisingData();
  AdvertisingData scan_rsp = CreateFakeAdvertisingData(21 /* size of ad */);

  // We expect FAST1 to fall back to FAST2 due to specification recommendation
  // (Vol 3, Part C, Appendix A) and lack of support for non-connectable
  // advertising with FAST1 parameters on certain controllers.
  adv_mgr()->StartAdvertising(CreateFakeAdvertisingData(),
                              CreateFakeAdvertisingData(/*packed_size=*/21),
                              /*connect_callback=*/nullptr,
                              AdvertisingInterval::FAST1,
                              /*extended_pdu=*/false,
                              /*anonymous=*/false,
                              /*include_tx_power_level=*/false,
                              /*address_type=*/std::nullopt,
                              GetSuccessCallback());
  RunUntilIdle();
  ASSERT_TRUE(last_status());
  ASSERT_TRUE(current_adv());
  EXPECT_EQ(kLEAdvertisingFastIntervalMin2, current_adv()->interval_min);
  EXPECT_EQ(kLEAdvertisingFastIntervalMax2, current_adv()->interval_max);
  DestroyAdvertisementInstance();

  adv_mgr()->StartAdvertising(CreateFakeAdvertisingData(),
                              CreateFakeAdvertisingData(/*packed_size=*/21),
                              /*connect_callback=*/nullptr,
                              AdvertisingInterval::FAST2,
                              /*extended_pdu=*/false,
                              /*anonymous=*/false,
                              /*include_tx_power_level=*/false,
                              /*address_type=*/std::nullopt,
                              GetSuccessCallback());
  RunUntilIdle();
  ASSERT_TRUE(last_status());
  ASSERT_TRUE(current_adv());
  EXPECT_EQ(kLEAdvertisingFastIntervalMin2, current_adv()->interval_min);
  EXPECT_EQ(kLEAdvertisingFastIntervalMax2, current_adv()->interval_max);
  DestroyAdvertisementInstance();

  adv_mgr()->StartAdvertising(CreateFakeAdvertisingData(),
                              CreateFakeAdvertisingData(/*packed_size=*/21),
                              /*connect_callback=*/nullptr,
                              AdvertisingInterval::SLOW,
                              /*extended_pdu=*/false,
                              /*anonymous=*/false,
                              /*include_tx_power_level=*/false,
                              /*address_type=*/std::nullopt,
                              GetSuccessCallback());
  RunUntilIdle();
  ASSERT_TRUE(last_status());
  ASSERT_TRUE(current_adv());
  EXPECT_EQ(kLEAdvertisingSlowIntervalMin, current_adv()->interval_min);
  EXPECT_EQ(kLEAdvertisingSlowIntervalMax, current_adv()->interval_max);
  DestroyAdvertisementInstance();
}

TEST_F(LowEnergyAdvertisingManagerTest, AdvertisePublicAddress) {
  adv_mgr()->StartAdvertising(CreateFakeAdvertisingData(),
                              CreateFakeAdvertisingData(/*packed_size=*/21),
                              NopConnectCallback,
                              AdvertisingInterval::FAST1,
                              /*extended_pdu=*/false,
                              /*anonymous=*/false,
                              /*include_tx_power_level=*/false,
                              DeviceAddress::Type::kLEPublic,
                              GetSuccessCallback());
  RunUntilIdle();
  ASSERT_TRUE(last_status());
  ASSERT_EQ(1u, ad_store().size());
  EXPECT_TRUE(adv_mgr()->advertising());

  // Verify that the advertiser uses the requested local address.
  EXPECT_EQ(kPublicAddress, ad_store().begin()->second.address);
}

TEST_F(LowEnergyAdvertisingManagerTest, AdvertiseRandomAddress) {
  adv_mgr()->StartAdvertising(CreateFakeAdvertisingData(),
                              CreateFakeAdvertisingData(/*packed_size=*/21),
                              NopConnectCallback,
                              AdvertisingInterval::FAST1,
                              /*extended_pdu=*/false,
                              /*anonymous=*/false,
                              /*include_tx_power_level=*/false,
                              /*address_type=*/std::nullopt,
                              GetSuccessCallback());
  RunUntilIdle();
  ASSERT_TRUE(last_status());
  ASSERT_EQ(1u, ad_store().size());
  EXPECT_TRUE(adv_mgr()->advertising());

  // Verify that the advertiser uses the requested local address.
  EXPECT_EQ(kRandomAddress, ad_store().begin()->second.address);
}

TEST_F(LowEnergyAdvertisingManagerTest, DestroyingInstanceStopsAdvertisement) {
  {
    AdvertisementInstance instance;
    adv_mgr()->StartAdvertising(AdvertisingData(),
                                AdvertisingData(),
                                /*connect_callback=*/nullptr,
                                AdvertisingInterval::FAST1,
                                /*extended_pdu=*/false,
                                /*anonymous=*/false,
                                /*include_tx_power_level=*/false,
                                /*address_type=*/std::nullopt,
                                [&](AdvertisementInstance i, auto status) {
                                  ASSERT_EQ(fit::ok(), status);
                                  instance = std::move(i);
                                });
    RunUntilIdle();
    EXPECT_TRUE(adv_mgr()->advertising());

    // Destroying |instance| should stop the advertisement.
  }

  RunUntilIdle();
  EXPECT_FALSE(adv_mgr()->advertising());
}

TEST_F(LowEnergyAdvertisingManagerTest, MovingIntoInstanceStopsAdvertisement) {
  AdvertisementInstance instance;
  adv_mgr()->StartAdvertising(AdvertisingData(),
                              AdvertisingData(),
                              /*connect_callback=*/nullptr,
                              AdvertisingInterval::FAST1,
                              /*extended_pdu=*/false,
                              /*anonymous=*/false,
                              /*include_tx_power_level=*/false,
                              /*address_type=*/std::nullopt,
                              [&](AdvertisementInstance i, auto status) {
                                ASSERT_EQ(fit::ok(), status);
                                instance = std::move(i);
                              });
  RunUntilIdle();
  EXPECT_TRUE(adv_mgr()->advertising());

  // Destroying |instance| by invoking the move assignment operator should
  // stop the advertisement.
  instance = {};
  RunUntilIdle();
  EXPECT_FALSE(adv_mgr()->advertising());
}

TEST_F(LowEnergyAdvertisingManagerTest,
       MovingInstanceTransfersOwnershipOfAdvertisement) {
  auto instance = std::make_unique<AdvertisementInstance>();
  adv_mgr()->StartAdvertising(AdvertisingData(),
                              AdvertisingData(),
                              /*connect_callback=*/nullptr,
                              AdvertisingInterval::FAST1,
                              /*extended_pdu=*/false,
                              /*anonymous=*/false,
                              /*include_tx_power_level=*/false,
                              /*address_type=*/std::nullopt,
                              [&](AdvertisementInstance i, auto status) {
                                ASSERT_EQ(fit::ok(), status);
                                *instance = std::move(i);
                              });
  RunUntilIdle();
  EXPECT_TRUE(adv_mgr()->advertising());

  // Moving |instance| should transfer the ownership of the advertisement
  // (assignment).
  {
    AdvertisementInstance move_assigned_instance = std::move(*instance);

    // Explicitly clearing the old instance should have no effect.
    *instance = {};
    RunUntilIdle();
    EXPECT_TRUE(adv_mgr()->advertising());

    *instance = std::move(move_assigned_instance);
  }

  // Advertisement should not stop when |move_assigned_instance| goes out of
  // scope as it no longer owns the advertisement.
  RunUntilIdle();
  EXPECT_TRUE(adv_mgr()->advertising());

  // Moving |instance| should transfer the ownership of the advertisement
  // (move-constructor).
  {
    AdvertisementInstance move_constructed_instance(std::move(*instance));

    // Explicitly destroying the old instance should have no effect.
    instance.reset();
    RunUntilIdle();
    EXPECT_TRUE(adv_mgr()->advertising());
  }

  // Advertisement should stop when |move_constructed_instance| goes out of
  // scope.
  RunUntilIdle();
  EXPECT_FALSE(adv_mgr()->advertising());
}

TEST_F(LowEnergyAdvertisingManagerTest, SetAdvertisingIntervalRange) {
  TestIntervalRange(
      [this](uint32_t min, uint32_t max) {
        adv_mgr()->set_slow_advertising_interval(min, max);
      },
      AdvertisingInterval::SLOW,
      /*connectable=*/false);
}

TEST_F(LowEnergyAdvertisingManagerTest, SetFastAdvertisingIntervalRange) {
  TestIntervalRange(
      [this](uint32_t min, uint32_t max) {
        adv_mgr()->set_fast_advertising_interval(min, max);
      },
      AdvertisingInterval::FAST2,
      /*connectable=*/false);
}

TEST_F(LowEnergyAdvertisingManagerTest, SetVeryFastAdvertisingIntervalRange) {
  TestIntervalRange(
      [this](uint32_t min, uint32_t max) {
        adv_mgr()->set_very_fast_advertising_interval(min, max);
      },
      AdvertisingInterval::FAST1,
      /*connectable=*/true);
}

TEST_F(LowEnergyAdvertisingManagerTest, SetExtendedAdvertisingIntervalRange) {
  advertiser()->set_extended(true);
  TestIntervalRange(
      [this](uint32_t min, uint32_t max) {
        adv_mgr()->set_slow_advertising_interval(min, max);
      },
      AdvertisingInterval::SLOW,
      /*connectable=*/false);
}

TEST_F(LowEnergyAdvertisingManagerTest,
       SetExtendedFastAdvertisingIntervalRange) {
  advertiser()->set_extended(true);
  TestIntervalRange(
      [this](uint32_t min, uint32_t max) {
        adv_mgr()->set_fast_advertising_interval(min, max);
      },
      AdvertisingInterval::FAST2,
      /*connectable=*/false);
}

TEST_F(LowEnergyAdvertisingManagerTest,
       SetExtendedVeryFastAdvertisingIntervalRange) {
  advertiser()->set_extended(true);
  TestIntervalRange(
      [this](uint32_t min, uint32_t max) {
        adv_mgr()->set_very_fast_advertising_interval(min, max);
      },
      AdvertisingInterval::FAST1,
      /*connectable=*/true);
}

TEST_F(LowEnergyAdvertisingManagerTest, SetAdvertisingTxPower) {
  TestAdvertisingTxPower(
      [this](int8_t power) { adv_mgr()->set_slow_adv_max_tx_power(power); },
      AdvertisingInterval::SLOW,
      /*connectable=*/false,
      /*expected_default_power=*/hci_spec::kLEAdvertisingTxPowerMin);

  TestAdvertisingTxPower(
      [this](int8_t power) { adv_mgr()->set_fast_adv_max_tx_power(power); },
      AdvertisingInterval::FAST2,
      /*connectable=*/false,
      /*expected_default_power=*/0);

  TestAdvertisingTxPower(
      [this](int8_t power) {
        adv_mgr()->set_very_fast_adv_max_tx_power(power);
      },
      AdvertisingInterval::FAST1,
      /*connectable=*/true,
      /*expected_default_power=*/hci_spec::kLEAdvertisingTxPowerMax);
}

}  // namespace
}  // namespace gap
}  // namespace bt
