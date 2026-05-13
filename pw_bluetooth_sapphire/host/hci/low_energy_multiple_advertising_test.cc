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

#include "pw_bluetooth_sapphire/internal/host/hci-spec/vendor_protocol.h"
#include "pw_bluetooth_sapphire/internal/host/hci/android_extended_low_energy_advertiser.h"
#include "pw_bluetooth_sapphire/internal/host/hci/extended_low_energy_advertiser.h"
#include "pw_bluetooth_sapphire/internal/host/testing/controller_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_controller.h"
#include "pw_bluetooth_sapphire/internal/host/testing/inspect_util.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"

// Multiple advertising is supported by the Bluetooth 5.0+ Core Specification as
// well as Android vendor extensions. This test file contains shared tests for
// both versions of LE Multiple Advertising.

namespace bt::hci {
namespace {

using bt::testing::FakeController;
using namespace inspect::testing;
using TestingBase = bt::testing::FakeDispatcherControllerTest<FakeController>;
using AdvertisingOptions = LowEnergyAdvertiser::AdvertisingOptions;
using LEAdvertisingState = FakeController::LEAdvertisingState;

constexpr AdvertisingIntervalRange kTestInterval(
    hci_spec::kLEAdvertisingIntervalMin, hci_spec::kLEAdvertisingIntervalMax);

const DeviceAddress kPublicAddress(DeviceAddress::Type::kLEPublic, {1});
const DeviceAddress kRandomAddress(DeviceAddress::Type::kLERandom, {2});

const AdvertisementId kFirstAdvertisementId = AdvertisementId(1);

template <typename T>
class LowEnergyMultipleAdvertisingTest : public TestingBase {
 public:
  LowEnergyMultipleAdvertisingTest() = default;
  ~LowEnergyMultipleAdvertisingTest() override = default;

 protected:
  void SetUp() override {
    TestingBase::SetUp();

    // ACL data channel needs to be present for production hci::Connection
    // objects.
    TestingBase::InitializeACLDataChannel(
        hci::DataBufferInfo(),
        hci::DataBufferInfo(hci_spec::kMaxACLPayloadSize, 10));

    FakeController::Settings settings;
    settings.ApplyExtendedLEConfig();
    this->test_device()->set_settings(settings);

    advertiser_ = std::unique_ptr<T>(CreateAdvertiserInternal());
    advertiser_->AttachInspect(inspector_.GetRoot());
  }

  void TearDown() override {
    advertiser_ = nullptr;
    this->test_device()->Stop();
    TestingBase::TearDown();
  }

  template <bool same = std::is_same_v<T, AndroidExtendedLowEnergyAdvertiser>>
  std::enable_if_t<same, AndroidExtendedLowEnergyAdvertiser>*
  CreateAdvertiserInternal() {
    return new AndroidExtendedLowEnergyAdvertiser(
        transport()->GetWeakPtr(), dispatcher(), max_advertisements_);
  }

  template <bool same = std::is_same_v<T, ExtendedLowEnergyAdvertiser>>
  std::enable_if_t<same, ExtendedLowEnergyAdvertiser>*
  CreateAdvertiserInternal() {
    return new ExtendedLowEnergyAdvertiser(
        transport()->GetWeakPtr(),
        dispatcher(),
        hci_spec::kMaxLEExtendedAdvertisingDataLength);
  }

  T* advertiser() const { return advertiser_.get(); }

  ResultFunction<AdvertisementId> MakeExpectSuccessCallback() {
    return [this](Result<AdvertisementId> status) {
      last_status_ = status;
      EXPECT_EQ(fit::ok(), status);
    };
  }

  ResultFunction<AdvertisementId> MakeExpectErrorCallback() {
    return [this](Result<AdvertisementId> status) {
      last_status_ = status;
      EXPECT_EQ(fit::failed(), status);
    };
  }

  static AdvertisingData GetExampleData(bool include_flags = true) {
    AdvertisingData result;

    std::string name = "fuchsia";
    EXPECT_TRUE(result.SetLocalName(name));

    uint16_t appearance = 0x1234;
    result.SetAppearance(appearance);

    EXPECT_LE(result.CalculateBlockSize(include_flags),
              hci_spec::kMaxLEAdvertisingDataLength);
    return result;
  }

  void SendMultipleAdvertisingPostConnectionEvents(
      hci_spec::ConnectionHandle conn_handle,
      hci_spec::AdvertisingHandle adv_handle) {
    if (std::is_same_v<T, AndroidExtendedLowEnergyAdvertiser>) {
      test_device()->SendAndroidLEMultipleAdvertisingStateChangeSubevent(
          conn_handle, adv_handle);
      return;
    }

    if (std::is_same_v<T, ExtendedLowEnergyAdvertiser>) {
      test_device()->SendLEAdvertisingSetTerminatedEvent(conn_handle,
                                                         adv_handle);
      return;
    }
  }

  void SimulateSetAdvertisingParametersFailure() {
    test_device()->SetDefaultAndroidResponseStatus(
        hci_spec::vendor::android::kLEMultiAdvt,
        hci_spec::vendor::android::kLEMultiAdvtSetAdvtParamSubopcode,
        pw::bluetooth::emboss::StatusCode::COMMAND_DISALLOWED);
    test_device()->SetDefaultResponseStatus(
        hci_spec::kLESetExtendedAdvertisingParameters,
        pw::bluetooth::emboss::StatusCode::COMMAND_DISALLOWED);
  }

  void ClearSetAdvertisingParametersFailure() {
    test_device()->ClearDefaultAndroidResponseStatus(
        hci_spec::vendor::android::kLEMultiAdvt,
        hci_spec::vendor::android::kLEMultiAdvtSetAdvtParamSubopcode);
    test_device()->ClearDefaultResponseStatus(
        hci_spec::kLESetExtendedAdvertisingParameters);
  }

  void SimulateEnableAdvertisingFailure() {
    test_device()->SetDefaultAndroidResponseStatus(
        hci_spec::vendor::android::kLEMultiAdvt,
        hci_spec::vendor::android::kLEMultiAdvtEnableSubopcode,
        pw::bluetooth::emboss::StatusCode::COMMAND_DISALLOWED);
    test_device()->SetDefaultResponseStatus(
        hci_spec::kLESetExtendedAdvertisingEnable,
        pw::bluetooth::emboss::StatusCode::COMMAND_DISALLOWED);
  }

  void ClearEnableAdvertisingFailure() {
    test_device()->ClearDefaultAndroidResponseStatus(
        hci_spec::vendor::android::kLEMultiAdvt,
        hci_spec::vendor::android::kLEMultiAdvtEnableSubopcode);
    test_device()->ClearDefaultResponseStatus(
        hci_spec::kLESetExtendedAdvertisingEnable);
  }

  std::optional<Result<AdvertisementId>> last_status() const {
    return last_status_;
  }

  std::optional<Result<AdvertisementId>> TakeLastStatus() {
    if (!last_status_) {
      return std::nullopt;
    }

    return std::exchange(last_status_, std::nullopt).value();
  }

  uint8_t max_advertisements() const { return max_advertisements_; }

  inspect::Inspector& inspector() { return inspector_; }

 private:
  std::unique_ptr<T> advertiser_;
  std::optional<Result<AdvertisementId>> last_status_;
  uint8_t max_advertisements_ = hci_spec::kMaxAdvertisingHandle + 1;
  inspect::Inspector inspector_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(LowEnergyMultipleAdvertisingTest);
};

using Implementations = ::testing::Types<ExtendedLowEnergyAdvertiser,
                                         AndroidExtendedLowEnergyAdvertiser>;
TYPED_TEST_SUITE(LowEnergyMultipleAdvertisingTest, Implementations);

TYPED_TEST(LowEnergyMultipleAdvertisingTest, AdvertisingHandlesExhausted) {
  this->test_device()->set_num_supported_advertising_sets(
      this->max_advertisements());

  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*init_extended_pdu=*/false,
                             /*init_anonymous=*/false,
                             /*init_include_tx_power_level=*/true);

  for (uint8_t i = 0; i < this->advertiser()->MaxAdvertisements(); i++) {
    this->advertiser()->StartAdvertising(
        DeviceAddress(DeviceAddress::Type::kLEPublic, {i}),
        ad,
        scan_data,
        options,
        /*connect_callback=*/nullptr,
        this->MakeExpectSuccessCallback());
    this->RunUntilIdle();
  }

  ASSERT_TRUE(this->TakeLastStatus());
  EXPECT_TRUE(this->advertiser()->IsAdvertising());
  EXPECT_EQ(this->advertiser()->MaxAdvertisements(),
            this->advertiser()->NumAdvertisements());

  this->advertiser()->StartAdvertising(
      DeviceAddress(DeviceAddress::Type::kLEPublic,
                    {hci_spec::kAdvertisingHandleMax + 1}),
      ad,
      scan_data,
      options,
      /*connect_callback=*/nullptr,
      this->MakeExpectErrorCallback());

  this->RunUntilIdle();
  ASSERT_TRUE(this->TakeLastStatus());
  EXPECT_TRUE(this->advertiser()->IsAdvertising());
  EXPECT_EQ(this->advertiser()->MaxAdvertisements(),
            this->advertiser()->NumAdvertisements());
}

TYPED_TEST(LowEnergyMultipleAdvertisingTest, SimultaneousAdvertisements) {
  this->test_device()->set_num_supported_advertising_sets(2);

  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();

  // start public address advertising
  AdvertisingOptions public_options(kTestInterval,
                                    kDefaultNoAdvFlags,
                                    /*init_extended_pdu=*/false,
                                    /*init_anonymous=*/false,
                                    /*init_include_tx_power_level=*/false);
  this->advertiser()->StartAdvertising(kPublicAddress,
                                       ad,
                                       scan_data,
                                       public_options,
                                       /*connect_callback=*/nullptr,
                                       this->MakeExpectSuccessCallback());
  this->RunUntilIdle();
  std::optional<hci_spec::AdvertisingHandle> handle_public_addr =
      this->advertiser()->LastUsedHandleForTesting();
  ASSERT_TRUE(handle_public_addr);
  std::optional<Result<AdvertisementId>> public_status = this->TakeLastStatus();
  ASSERT_TRUE(public_status.has_value());
  ASSERT_TRUE(public_status->is_ok());
  AdvertisementId public_id = public_status->value();

  // start random address advertising
  constexpr AdvertisingIntervalRange random_interval(
      hci_spec::kLEAdvertisingIntervalMin + 1u,
      hci_spec::kLEAdvertisingIntervalMax - 1u);
  AdvertisingOptions random_options(random_interval,
                                    kDefaultNoAdvFlags,
                                    /*init_extended_pdu=*/false,
                                    /*init_anonymous=*/false,
                                    /*init_include_tx_power_level=*/false);
  this->advertiser()->StartAdvertising(kRandomAddress,
                                       ad,
                                       scan_data,
                                       random_options,
                                       /*connect_callback=*/nullptr,
                                       this->MakeExpectSuccessCallback());
  this->RunUntilIdle();
  std::optional<hci_spec::AdvertisingHandle> handle_random_addr =
      this->advertiser()->LastUsedHandleForTesting();
  ASSERT_TRUE(handle_random_addr);

  std::optional<Result<AdvertisementId>> random_status = this->TakeLastStatus();
  ASSERT_TRUE(random_status.has_value());
  ASSERT_TRUE(random_status->is_ok());
  AdvertisementId random_id = random_status->value();

  // check everything is correct
  EXPECT_EQ(2u, this->advertiser()->NumAdvertisements());
  EXPECT_TRUE(this->advertiser()->IsAdvertising());
  EXPECT_TRUE(this->advertiser()->IsAdvertising(public_id));
  EXPECT_TRUE(this->advertiser()->IsAdvertising(random_id));

  const LEAdvertisingState& public_addr_state =
      this->test_device()->extended_advertising_state(
          handle_public_addr.value());
  const LEAdvertisingState& random_addr_state =
      this->test_device()->extended_advertising_state(
          handle_random_addr.value());

  EXPECT_TRUE(public_addr_state.enabled);
  EXPECT_TRUE(random_addr_state.enabled);
  EXPECT_EQ(pw::bluetooth::emboss::LEOwnAddressType::PUBLIC,
            public_addr_state.own_address_type);
  EXPECT_EQ(pw::bluetooth::emboss::LEOwnAddressType::RANDOM,
            random_addr_state.own_address_type);
  ASSERT_TRUE(random_addr_state.random_address.has_value());
  EXPECT_EQ(kRandomAddress, *random_addr_state.random_address);
  EXPECT_EQ(hci_spec::kLEAdvertisingIntervalMin,
            public_addr_state.interval_min);
  EXPECT_EQ(hci_spec::kLEAdvertisingIntervalMax,
            public_addr_state.interval_max);
  EXPECT_EQ(hci_spec::kLEAdvertisingIntervalMin + 1u,
            random_addr_state.interval_min);
  EXPECT_EQ(hci_spec::kLEAdvertisingIntervalMax - 1u,
            random_addr_state.interval_max);
}

// Ensure that we can start multiple advertisements that use random addresses,
// with a different address for each.
TYPED_TEST(LowEnergyMultipleAdvertisingTest, SimultaneousRandomAdvertisements) {
  const DeviceAddress kRandomAddress1(DeviceAddress::Type::kLERandom, {0x55});
  const DeviceAddress kRandomAddress2(DeviceAddress::Type::kLERandom, {0xaa});
  this->test_device()->set_num_supported_advertising_sets(2);

  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();

  // start first random address advertising
  AdvertisingOptions options1(kTestInterval,
                              kDefaultNoAdvFlags,
                              /*init_extended_pdu=*/false,
                              /*init_anonymous=*/false,
                              /*init_include_tx_power_level=*/false);
  this->advertiser()->StartAdvertising(kRandomAddress1,
                                       ad,
                                       scan_data,
                                       options1,
                                       /*connect_callback=*/nullptr,
                                       this->MakeExpectSuccessCallback());
  this->RunUntilIdle();
  std::optional<hci_spec::AdvertisingHandle> handle1 =
      this->advertiser()->LastUsedHandleForTesting();
  ASSERT_TRUE(handle1);
  std::optional<Result<AdvertisementId>> status_1 = this->TakeLastStatus();
  ASSERT_TRUE(status_1.has_value());
  ASSERT_TRUE(status_1->is_ok());
  AdvertisementId adv_id_1 = status_1->value();

  // start second random address advertising
  constexpr AdvertisingIntervalRange random_interval(
      hci_spec::kLEAdvertisingIntervalMin + 1u,
      hci_spec::kLEAdvertisingIntervalMax - 1u);
  AdvertisingOptions options2(random_interval,
                              kDefaultNoAdvFlags,
                              /*init_extended_pdu=*/false,
                              /*init_anonymous=*/false,
                              /*init_include_tx_power_level=*/false);
  this->advertiser()->StartAdvertising(kRandomAddress2,
                                       ad,
                                       scan_data,
                                       options2,
                                       /*connect_callback=*/nullptr,
                                       this->MakeExpectSuccessCallback());
  this->RunUntilIdle();
  std::optional<hci_spec::AdvertisingHandle> handle2 =
      this->advertiser()->LastUsedHandleForTesting();
  ASSERT_TRUE(handle2);
  std::optional<Result<AdvertisementId>> status_2 = this->TakeLastStatus();
  ASSERT_TRUE(status_2.has_value());
  ASSERT_TRUE(status_2->is_ok());
  AdvertisementId adv_id_2 = status_2->value();

  // check everything is correct
  EXPECT_EQ(2u, this->advertiser()->NumAdvertisements());
  EXPECT_TRUE(this->advertiser()->IsAdvertising());
  EXPECT_TRUE(this->advertiser()->IsAdvertising(adv_id_1));
  EXPECT_TRUE(this->advertiser()->IsAdvertising(adv_id_2));

  const LEAdvertisingState& addr_state1 =
      this->test_device()->extended_advertising_state(handle1.value());
  const LEAdvertisingState& addr_state2 =
      this->test_device()->extended_advertising_state(handle2.value());

  EXPECT_TRUE(addr_state1.enabled);
  EXPECT_TRUE(addr_state2.enabled);
  EXPECT_EQ(pw::bluetooth::emboss::LEOwnAddressType::RANDOM,
            addr_state1.own_address_type);
  ASSERT_TRUE(addr_state1.random_address.has_value());
  EXPECT_EQ(kRandomAddress1, *addr_state1.random_address);
  EXPECT_EQ(pw::bluetooth::emboss::LEOwnAddressType::RANDOM,
            addr_state2.own_address_type);
  ASSERT_TRUE(addr_state2.random_address.has_value());
  EXPECT_EQ(kRandomAddress2, *addr_state2.random_address);
  EXPECT_EQ(hci_spec::kLEAdvertisingIntervalMin, addr_state1.interval_min);
  EXPECT_EQ(hci_spec::kLEAdvertisingIntervalMax, addr_state1.interval_max);
  EXPECT_EQ(hci_spec::kLEAdvertisingIntervalMin + 1u, addr_state2.interval_min);
  EXPECT_EQ(hci_spec::kLEAdvertisingIntervalMax - 1u, addr_state2.interval_max);
}

TYPED_TEST(LowEnergyMultipleAdvertisingTest,
           StopAdvertisingAllAdvertisementsStopped) {
  this->test_device()->set_num_supported_advertising_sets(2);

  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();

  // start public address advertising
  AdvertisingOptions public_options(kTestInterval,
                                    kDefaultNoAdvFlags,
                                    /*init_extended_pdu=*/false,
                                    /*init_anonymous=*/false,
                                    /*init_include_tx_power_level=*/false);
  this->advertiser()->StartAdvertising(kPublicAddress,
                                       ad,
                                       scan_data,
                                       public_options,
                                       /*connect_callback=*/nullptr,
                                       this->MakeExpectSuccessCallback());
  this->RunUntilIdle();
  std::optional<hci_spec::AdvertisingHandle> handle_public_addr =
      this->advertiser()->LastUsedHandleForTesting();
  ASSERT_TRUE(handle_public_addr);
  std::optional<Result<AdvertisementId>> public_status = this->TakeLastStatus();
  ASSERT_TRUE(public_status.has_value());
  ASSERT_TRUE(public_status->is_ok());
  AdvertisementId public_adv_id = public_status->value();

  // start random address advertising
  constexpr AdvertisingIntervalRange random_interval(
      hci_spec::kLEAdvertisingIntervalMin + 1u,
      hci_spec::kLEAdvertisingIntervalMax - 1u);
  AdvertisingOptions random_options(random_interval,
                                    kDefaultNoAdvFlags,
                                    /*init_extended_pdu=*/false,
                                    /*init_anonymous=*/false,
                                    /*init_include_tx_power_level=*/false);
  this->advertiser()->StartAdvertising(kRandomAddress,
                                       ad,
                                       scan_data,
                                       random_options,
                                       /*connect_callback=*/nullptr,
                                       this->MakeExpectSuccessCallback());
  this->RunUntilIdle();
  std::optional<hci_spec::AdvertisingHandle> handle_random_addr =
      this->advertiser()->LastUsedHandleForTesting();
  ASSERT_TRUE(handle_random_addr);
  std::optional<Result<AdvertisementId>> random_status = this->TakeLastStatus();
  ASSERT_TRUE(random_status.has_value());
  ASSERT_TRUE(random_status->is_ok());
  AdvertisementId random_adv_id = random_status->value();

  // check everything is correct
  EXPECT_EQ(2u, this->advertiser()->NumAdvertisements());
  EXPECT_TRUE(this->advertiser()->IsAdvertising());
  EXPECT_TRUE(this->advertiser()->IsAdvertising(public_adv_id));
  EXPECT_TRUE(this->advertiser()->IsAdvertising(random_adv_id));

  // Stop advertising
  this->advertiser()->StopAdvertising();
  this->RunUntilIdle();

  // Check that advertiser and controller both report not advertising
  EXPECT_EQ(0u, this->advertiser()->NumAdvertisements());
  EXPECT_FALSE(this->advertiser()->IsAdvertising());
  EXPECT_FALSE(this->advertiser()->IsAdvertising(public_adv_id));
  EXPECT_FALSE(this->advertiser()->IsAdvertising(random_adv_id));

  const LEAdvertisingState& public_addr_state =
      this->test_device()->extended_advertising_state(
          handle_public_addr.value());
  const LEAdvertisingState& random_addr_state =
      this->test_device()->extended_advertising_state(
          handle_random_addr.value());

  constexpr uint8_t blank[hci_spec::kMaxLEAdvertisingDataLength] = {0};

  EXPECT_FALSE(public_addr_state.enabled);
  EXPECT_EQ(0,
            std::memcmp(blank,
                        public_addr_state.data,
                        hci_spec::kMaxLEAdvertisingDataLength));
  EXPECT_EQ(0, public_addr_state.data_length);
  EXPECT_EQ(0,
            std::memcmp(blank,
                        public_addr_state.data,
                        hci_spec::kMaxLEAdvertisingDataLength));
  EXPECT_EQ(0, public_addr_state.scan_rsp_length);

  EXPECT_FALSE(random_addr_state.enabled);
  EXPECT_EQ(0,
            std::memcmp(blank,
                        random_addr_state.data,
                        hci_spec::kMaxLEAdvertisingDataLength));
  EXPECT_EQ(0, random_addr_state.data_length);
  EXPECT_EQ(0,
            std::memcmp(blank,
                        random_addr_state.data,
                        hci_spec::kMaxLEAdvertisingDataLength));
  EXPECT_EQ(0, random_addr_state.scan_rsp_length);
}

TYPED_TEST(LowEnergyMultipleAdvertisingTest,
           StopAdvertisingSingleAdvertisement) {
  this->test_device()->set_num_supported_advertising_sets(2);

  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();

  // start public address advertising
  AdvertisingOptions public_options(kTestInterval,
                                    kDefaultNoAdvFlags,
                                    /*init_extended_pdu=*/false,
                                    /*init_anonymous=*/false,
                                    /*init_include_tx_power_level=*/false);
  this->advertiser()->StartAdvertising(kPublicAddress,
                                       ad,
                                       scan_data,
                                       public_options,
                                       /*connect_callback=*/nullptr,
                                       this->MakeExpectSuccessCallback());
  this->RunUntilIdle();
  std::optional<hci_spec::AdvertisingHandle> handle_public_addr =
      this->advertiser()->LastUsedHandleForTesting();
  ASSERT_TRUE(handle_public_addr);
  ASSERT_TRUE(this->last_status());
  ASSERT_TRUE(this->last_status()->is_ok());
  AdvertisementId public_adv_id = this->last_status()->value();

  // start random address advertising
  constexpr AdvertisingIntervalRange random_interval(
      hci_spec::kLEAdvertisingIntervalMin + 1u,
      hci_spec::kLEAdvertisingIntervalMax - 1u);
  AdvertisingOptions random_options(random_interval,
                                    kDefaultNoAdvFlags,
                                    /*init_extended_pdu=*/false,
                                    /*init_anonymous=*/false,
                                    /*init_include_tx_power_level=*/false);
  this->advertiser()->StartAdvertising(kRandomAddress,
                                       ad,
                                       scan_data,
                                       random_options,
                                       /*connect_callback=*/nullptr,
                                       this->MakeExpectSuccessCallback());
  this->RunUntilIdle();
  std::optional<hci_spec::AdvertisingHandle> handle_random_addr =
      this->advertiser()->LastUsedHandleForTesting();
  ASSERT_TRUE(handle_random_addr);
  ASSERT_TRUE(this->last_status());
  ASSERT_TRUE(this->last_status()->is_ok());
  AdvertisementId random_adv_id = this->last_status()->value();

  // check everything is correct
  EXPECT_TRUE(this->advertiser()->IsAdvertising());
  EXPECT_EQ(2u, this->advertiser()->NumAdvertisements());
  EXPECT_TRUE(this->advertiser()->IsAdvertising(public_adv_id));
  EXPECT_TRUE(this->advertiser()->IsAdvertising(random_adv_id));

  // Stop advertising the random address
  this->advertiser()->StopAdvertising(random_adv_id);
  this->RunUntilIdle();

  // Check that advertiser and controller both report the same advertising state
  EXPECT_TRUE(this->advertiser()->IsAdvertising());
  EXPECT_EQ(1u, this->advertiser()->NumAdvertisements());
  EXPECT_TRUE(this->advertiser()->IsAdvertising(public_adv_id));
  EXPECT_FALSE(this->advertiser()->IsAdvertising(random_adv_id));

  constexpr uint8_t blank[hci_spec::kMaxLEAdvertisingDataLength] = {0};

  {
    const LEAdvertisingState& public_addr_state =
        this->test_device()->extended_advertising_state(
            handle_public_addr.value());
    const LEAdvertisingState& random_addr_state =
        this->test_device()->extended_advertising_state(
            handle_random_addr.value());

    EXPECT_TRUE(public_addr_state.enabled);
    EXPECT_NE(0,
              std::memcmp(blank,
                          public_addr_state.data,
                          hci_spec::kMaxLEAdvertisingDataLength));
    EXPECT_NE(0, public_addr_state.data_length);
    EXPECT_NE(0,
              std::memcmp(blank,
                          public_addr_state.data,
                          hci_spec::kMaxLEAdvertisingDataLength));
    EXPECT_NE(0, public_addr_state.scan_rsp_length);

    EXPECT_FALSE(random_addr_state.enabled);
    EXPECT_EQ(0,
              std::memcmp(blank,
                          random_addr_state.data,
                          hci_spec::kMaxLEAdvertisingDataLength));
    EXPECT_EQ(0, random_addr_state.data_length);
    EXPECT_EQ(0,
              std::memcmp(blank,
                          random_addr_state.data,
                          hci_spec::kMaxLEAdvertisingDataLength));
    EXPECT_EQ(0, random_addr_state.scan_rsp_length);
  }

  // stop advertising the public address
  this->advertiser()->StopAdvertising(public_adv_id);
  this->RunUntilIdle();

  {
    const LEAdvertisingState& public_addr_state =
        this->test_device()->extended_advertising_state(
            handle_public_addr.value());
    const LEAdvertisingState& random_addr_state =
        this->test_device()->extended_advertising_state(
            handle_random_addr.value());

    // Check that advertiser and controller both report the same advertising
    // state
    EXPECT_FALSE(this->advertiser()->IsAdvertising());
    EXPECT_EQ(0u, this->advertiser()->NumAdvertisements());
    EXPECT_FALSE(this->advertiser()->IsAdvertising(public_adv_id));
    EXPECT_FALSE(this->advertiser()->IsAdvertising(random_adv_id));

    EXPECT_FALSE(public_addr_state.enabled);
    EXPECT_EQ(0,
              std::memcmp(blank,
                          public_addr_state.data,
                          hci_spec::kMaxLEAdvertisingDataLength));
    EXPECT_EQ(0, public_addr_state.data_length);
    EXPECT_EQ(0,
              std::memcmp(blank,
                          public_addr_state.data,
                          hci_spec::kMaxLEAdvertisingDataLength));
    EXPECT_EQ(0, public_addr_state.scan_rsp_length);

    EXPECT_FALSE(random_addr_state.enabled);
    EXPECT_EQ(0,
              std::memcmp(blank,
                          random_addr_state.data,
                          hci_spec::kMaxLEAdvertisingDataLength));
    EXPECT_EQ(0, random_addr_state.data_length);
    EXPECT_EQ(0,
              std::memcmp(blank,
                          random_addr_state.data,
                          hci_spec::kMaxLEAdvertisingDataLength));
    EXPECT_EQ(0, random_addr_state.scan_rsp_length);
  }
}

TYPED_TEST(LowEnergyMultipleAdvertisingTest, SuccessiveAdvertisingCalls) {
  this->test_device()->set_num_supported_advertising_sets(2);

  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*init_extended_pdu=*/false,
                             /*init_anonymous=*/false,
                             /*init_include_tx_power_level=*/false);

  std::optional<Result<AdvertisementId>> start_result_0;
  this->advertiser()->StartAdvertising(
      kPublicAddress,
      ad,
      scan_data,
      options,
      /*connect_callback=*/nullptr,
      [&](Result<AdvertisementId> result) { start_result_0 = result; });
  std::optional<Result<AdvertisementId>> start_result_1;
  this->advertiser()->StartAdvertising(
      kRandomAddress,
      ad,
      scan_data,
      options,
      /*connect_callback=*/nullptr,
      [&](Result<AdvertisementId> result) { start_result_1 = result; });

  this->RunUntilIdle();
  ASSERT_TRUE(start_result_0);
  ASSERT_TRUE(start_result_0.value().is_ok());
  ASSERT_TRUE(start_result_1);
  ASSERT_TRUE(start_result_1.value().is_ok());
  EXPECT_TRUE(this->advertiser()->IsAdvertising());
  EXPECT_EQ(2u, this->advertiser()->NumAdvertisements());
  EXPECT_TRUE(this->advertiser()->IsAdvertising(start_result_0->value()));
  EXPECT_TRUE(this->advertiser()->IsAdvertising(start_result_1->value()));

  this->advertiser()->StopAdvertising(start_result_0->value());
  this->advertiser()->StopAdvertising(start_result_1->value());

  this->RunUntilIdle();
  EXPECT_FALSE(this->advertiser()->IsAdvertising());
  EXPECT_EQ(0u, this->advertiser()->NumAdvertisements());
  EXPECT_FALSE(this->advertiser()->IsAdvertising(start_result_0->value()));
  EXPECT_FALSE(this->advertiser()->IsAdvertising(start_result_1->value()));
}

TYPED_TEST(LowEnergyMultipleAdvertisingTest, InterleavedAdvertisingCalls) {
  this->test_device()->set_num_supported_advertising_sets(
      this->max_advertisements());

  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*init_extended_pdu=*/false,
                             /*init_anonymous=*/false,
                             /*init_include_tx_power_level=*/false);

  std::optional<Result<AdvertisementId>> result_0;
  this->advertiser()->StartAdvertising(
      kPublicAddress,
      ad,
      scan_data,
      options,
      /*connect_callback=*/nullptr,
      [&](Result<AdvertisementId> result) { result_0 = result; });
  std::optional<hci_spec::AdvertisingHandle> handle_0 =
      this->advertiser()->LastUsedHandleForTesting();
  ASSERT_TRUE(handle_0);
  this->advertiser()->StopAdvertising();
  this->advertiser()->StartAdvertising(kPublicAddress,
                                       ad,
                                       scan_data,
                                       options,
                                       /*connect_callback=*/nullptr,
                                       this->MakeExpectSuccessCallback());

  this->RunUntilIdle();
  ASSERT_TRUE(result_0.has_value());
  ASSERT_TRUE(result_0->is_error());

  std::optional<hci_spec::AdvertisingHandle> handle_1 =
      this->advertiser()->LastUsedHandleForTesting();
  ASSERT_TRUE(handle_1);
  std::optional<Result<AdvertisementId>> status_1 = this->TakeLastStatus();
  ASSERT_TRUE(status_1.has_value());
  ASSERT_TRUE(status_1->is_ok());
  AdvertisementId adv_id_1 = status_1->value();

  EXPECT_TRUE(this->advertiser()->IsAdvertising());
  EXPECT_EQ(1u, this->advertiser()->NumAdvertisements());
  EXPECT_FALSE(this->advertiser()->IsAdvertising(kFirstAdvertisementId));
  EXPECT_TRUE(this->advertiser()->IsAdvertising(adv_id_1));
}

TYPED_TEST(LowEnergyMultipleAdvertisingTest, StopWhileStarting) {
  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*init_extended_pdu=*/false,
                             /*init_anonymous=*/false,
                             /*init_include_tx_power_level=*/false);

  this->advertiser()->StartAdvertising(kPublicAddress,
                                       ad,
                                       scan_data,
                                       options,
                                       /*connect_callback=*/nullptr,
                                       this->MakeExpectErrorCallback());
  std::optional<hci_spec::AdvertisingHandle> adv_handle =
      this->advertiser()->LastUsedHandleForTesting();
  ASSERT_TRUE(adv_handle);

  this->advertiser()->StopAdvertising();

  this->RunUntilIdle();
  EXPECT_TRUE(this->TakeLastStatus());

  std::optional<hci_spec::AdvertisingHandle> handle =
      this->advertiser()->LastUsedHandleForTesting();
  ASSERT_TRUE(handle);

  EXPECT_FALSE(
      this->test_device()->extended_advertising_state(handle.value()).enabled);
}

TYPED_TEST(LowEnergyMultipleAdvertisingTest,
           MultipleAdvertisementsWithSameAddressWithConnections) {
  this->test_device()->set_num_supported_advertising_sets(2);

  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();

  AdvertisingOptions options_0(kTestInterval,
                               kDefaultNoAdvFlags,
                               /*init_extended_pdu=*/false,
                               /*init_anonymous=*/false,
                               /*init_include_tx_power_level=*/false);
  std::optional<Result<AdvertisementId>> result_0;
  std::unique_ptr<LowEnergyConnection> link_0;
  auto conn_cb_0 = [&link_0](auto, auto cb_link) {
    link_0 = std::move(cb_link);
  };
  this->advertiser()->StartAdvertising(
      kPublicAddress,
      ad,
      scan_data,
      options_0,
      std::move(conn_cb_0),
      [&](Result<AdvertisementId> result) { result_0 = result; });
  this->RunUntilIdle();
  ASSERT_TRUE(result_0);
  ASSERT_TRUE(result_0->is_ok());
  AdvertisementId adv_id_0 = result_0->value();

  std::optional<hci_spec::AdvertisingHandle> adv_handle_0 =
      this->advertiser()->LastUsedHandleForTesting();
  ASSERT_TRUE(adv_handle_0);

  constexpr AdvertisingIntervalRange interval_1(
      hci_spec::kLEAdvertisingIntervalMin + 1u,
      hci_spec::kLEAdvertisingIntervalMax - 1u);
  AdvertisingOptions options_1(interval_1,
                               kDefaultNoAdvFlags,
                               /*init_extended_pdu=*/false,
                               /*init_anonymous=*/false,
                               /*init_include_tx_power_level=*/false);
  std::optional<Result<AdvertisementId>> result_1;
  std::unique_ptr<LowEnergyConnection> link_1;
  auto conn_cb_1 = [&link_1](auto, auto cb_link) {
    link_1 = std::move(cb_link);
  };
  this->advertiser()->StartAdvertising(
      kPublicAddress,
      ad,
      scan_data,
      options_1,
      std::move(conn_cb_1),
      [&](Result<AdvertisementId> result) { result_1 = result; });
  this->RunUntilIdle();
  ASSERT_TRUE(result_1);
  ASSERT_TRUE(result_1->is_ok());
  AdvertisementId adv_id_1 = result_1->value();

  std::optional<hci_spec::AdvertisingHandle> adv_handle_1 =
      this->advertiser()->LastUsedHandleForTesting();
  ASSERT_TRUE(adv_handle_1);

  EXPECT_EQ(2u, this->advertiser()->NumAdvertisements());
  EXPECT_TRUE(this->advertiser()->IsAdvertising());
  EXPECT_TRUE(this->advertiser()->IsAdvertising(adv_id_0));
  EXPECT_TRUE(this->advertiser()->IsAdvertising(adv_id_1));

  const LEAdvertisingState& adv_state_0 =
      this->test_device()->extended_advertising_state(adv_handle_0.value());
  EXPECT_TRUE(adv_state_0.enabled);
  EXPECT_EQ(pw::bluetooth::emboss::LEOwnAddressType::PUBLIC,
            adv_state_0.own_address_type);
  EXPECT_EQ(hci_spec::kLEAdvertisingIntervalMin, adv_state_0.interval_min);
  EXPECT_EQ(hci_spec::kLEAdvertisingIntervalMax, adv_state_0.interval_max);

  const LEAdvertisingState& adv_state_1 =
      this->test_device()->extended_advertising_state(adv_handle_1.value());
  EXPECT_TRUE(adv_state_1.enabled);
  EXPECT_EQ(pw::bluetooth::emboss::LEOwnAddressType::PUBLIC,
            adv_state_1.own_address_type);
  EXPECT_EQ(hci_spec::kLEAdvertisingIntervalMin + 1u, adv_state_1.interval_min);
  EXPECT_EQ(hci_spec::kLEAdvertisingIntervalMax - 1u, adv_state_1.interval_max);

  const hci_spec::ConnectionHandle conn_handle_0 = 0x0001;
  const hci_spec::ConnectionHandle conn_handle_1 = 0x0002;

  this->advertiser()->OnIncomingConnection(
      conn_handle_0,
      pw::bluetooth::emboss::ConnectionRole::PERIPHERAL,
      kPublicAddress,
      hci_spec::LEConnectionParameters());
  this->SendMultipleAdvertisingPostConnectionEvents(conn_handle_0,
                                                    adv_handle_0.value());
  this->RunUntilIdle();
  ASSERT_TRUE(link_0);
  EXPECT_EQ(conn_handle_0, link_0->handle());
  EXPECT_EQ(kPublicAddress, link_0->local_address());
  EXPECT_FALSE(this->advertiser()->IsAdvertising(adv_id_0));
  ASSERT_FALSE(link_1);
  EXPECT_TRUE(this->advertiser()->IsAdvertising(adv_id_1));

  this->advertiser()->OnIncomingConnection(
      conn_handle_1,
      pw::bluetooth::emboss::ConnectionRole::PERIPHERAL,
      kPublicAddress,
      hci_spec::LEConnectionParameters());
  this->SendMultipleAdvertisingPostConnectionEvents(conn_handle_1,
                                                    adv_handle_1.value());
  this->RunUntilIdle();
  ASSERT_TRUE(link_1);
  EXPECT_EQ(conn_handle_1, link_1->handle());
  EXPECT_EQ(kPublicAddress, link_1->local_address());
  EXPECT_FALSE(this->advertiser()->IsAdvertising(adv_id_1));
}

#ifndef NINSPECT
TYPED_TEST(LowEnergyMultipleAdvertisingTest, Inspect) {
  auto map_matcher =
      AllOf(NodeMatches(AllOf(NameMatches("advertising_handle_map"))));

  auto advertiser_matcher =
      AllOf(NodeMatches(AllOf(NameMatches("low_energy_advertiser"))),
            ChildrenMatch(ElementsAre(map_matcher)));

  EXPECT_THAT(inspect::ReadFromVmo(this->inspector().DuplicateVmo()).value(),
              ChildrenMatch(ElementsAre(advertiser_matcher)));
}
#endif  // NINSPECT

TYPED_TEST(
    LowEnergyMultipleAdvertisingTest,
    StartAdvertisingFailureDoesNotLeakHandleOnSetAdvertisingParametersFailure) {
  this->test_device()->set_num_supported_advertising_sets(
      this->max_advertisements());

  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*init_extended_pdu=*/false,
                             /*init_anonymous=*/false,
                             /*init_include_tx_power_level=*/true);

  this->SimulateSetAdvertisingParametersFailure();
  this->advertiser()->StartAdvertising(
      DeviceAddress(DeviceAddress::Type::kLEPublic, {0xFF}),
      ad,
      scan_data,
      options,
      /*connect_callback=*/nullptr,
      this->MakeExpectErrorCallback());

  this->RunUntilIdle();
  ASSERT_TRUE(this->TakeLastStatus());
  EXPECT_FALSE(this->advertiser()->IsAdvertising());
  this->ClearSetAdvertisingParametersFailure();

  // Ensure the handle was not leaked by advertising the max number of
  // advertisements.
  for (uint8_t i = 0; i < this->advertiser()->MaxAdvertisements(); i++) {
    this->advertiser()->StartAdvertising(
        DeviceAddress(DeviceAddress::Type::kLEPublic, {i}),
        ad,
        scan_data,
        options,
        /*connect_callback=*/nullptr,
        this->MakeExpectSuccessCallback());
    this->RunUntilIdle();
  }

  ASSERT_TRUE(this->TakeLastStatus());
  EXPECT_TRUE(this->advertiser()->IsAdvertising());
  EXPECT_EQ(this->advertiser()->MaxAdvertisements(),
            this->advertiser()->NumAdvertisements());
}

TYPED_TEST(LowEnergyMultipleAdvertisingTest,
           StartAdvertisingFailureDoesNotLeakHandleOnEnableFailure) {
  this->test_device()->set_num_supported_advertising_sets(
      this->max_advertisements());

  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*init_extended_pdu=*/false,
                             /*init_anonymous=*/false,
                             /*init_include_tx_power_level=*/true);

  this->SimulateEnableAdvertisingFailure();
  this->advertiser()->StartAdvertising(
      DeviceAddress(DeviceAddress::Type::kLEPublic, {0xFF}),
      ad,
      scan_data,
      options,
      /*connect_callback=*/nullptr,
      this->MakeExpectErrorCallback());

  this->RunUntilIdle();
  ASSERT_TRUE(this->TakeLastStatus());
  EXPECT_FALSE(this->advertiser()->IsAdvertising());
  this->ClearEnableAdvertisingFailure();

  // Ensure the handle was not leaked by advertising the max number of
  // advertisements.
  for (uint8_t i = 0; i < this->advertiser()->MaxAdvertisements(); i++) {
    this->advertiser()->StartAdvertising(
        DeviceAddress(DeviceAddress::Type::kLEPublic, {i}),
        ad,
        scan_data,
        options,
        /*connect_callback=*/nullptr,
        this->MakeExpectSuccessCallback());
    this->RunUntilIdle();
  }

  ASSERT_TRUE(this->TakeLastStatus());
  EXPECT_TRUE(this->advertiser()->IsAdvertising());
  EXPECT_EQ(this->advertiser()->MaxAdvertisements(),
            this->advertiser()->NumAdvertisements());
}

using AndroidLowEnergyMultipleAdvertisingTest =
    LowEnergyMultipleAdvertisingTest<AndroidExtendedLowEnergyAdvertiser>;

TEST_F(AndroidLowEnergyMultipleAdvertisingTest, StartAndStopWithTxPower) {
  this->test_device()->set_num_supported_advertising_sets(1);

  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*init_extended_pdu=*/false,
                             /*init_anonymous=*/false,
                             /*init_include_tx_power_level=*/true);

  this->advertiser()->StartAdvertising(kPublicAddress,
                                       ad,
                                       scan_data,
                                       options,
                                       /*connect_callback=*/nullptr,
                                       this->MakeExpectSuccessCallback());
  this->RunUntilIdle();
  std::optional<hci_spec::AdvertisingHandle> handle =
      this->advertiser()->LastUsedHandleForTesting();
  ASSERT_TRUE(handle);
  std::optional<Result<AdvertisementId>> status = this->TakeLastStatus();
  ASSERT_TRUE(status.has_value());
  ASSERT_TRUE(status->is_ok());
  AdvertisementId id = status->value();

  EXPECT_TRUE(this->advertiser()->IsAdvertising());
  EXPECT_TRUE(this->advertiser()->IsAdvertising(id));

  const LEAdvertisingState& addr_state =
      this->test_device()->extended_advertising_state(handle.value());

  EXPECT_TRUE(addr_state.enabled);

  // The fake controller returns 9 dBm for LE Read Advertising Channel Tx Power.
  ad.SetTxPower(9);
  scan_data.SetTxPower(9);

  size_t expected_ad_size = ad.CalculateBlockSize(/*include_flags=*/true);
  DynamicByteBuffer expected_ad(expected_ad_size);
  ad.WriteBlock(&expected_ad, kDefaultNoAdvFlags);

  BufferView actual_ad(addr_state.data, addr_state.data_length);
  EXPECT_TRUE(ContainersEqual(expected_ad, actual_ad));

  size_t expected_scan_size =
      scan_data.CalculateBlockSize(/*include_flags=*/false);
  DynamicByteBuffer expected_scan(expected_scan_size);
  scan_data.WriteBlock(&expected_scan, std::nullopt);

  BufferView actual_scan(addr_state.scan_rsp_data, addr_state.scan_rsp_length);
  EXPECT_TRUE(ContainersEqual(expected_scan, actual_scan));

  this->advertiser()->StopAdvertising(id);
  this->RunUntilIdle();
  EXPECT_FALSE(this->advertiser()->IsAdvertising());
}

// Ensure that when starting advertising with TX power level included, if the
// controller returns an error for the read TX power level command, advertising
// fails to start and the error is propagated.
TEST_F(AndroidLowEnergyMultipleAdvertisingTest,
       StartAdvertisingFailureTxPowerReadError) {
  this->test_device()->set_num_supported_advertising_sets(1);

  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*init_extended_pdu=*/false,
                             /*init_anonymous=*/false,
                             /*init_include_tx_power_level=*/true);

  this->test_device()->SetDefaultResponseStatus(
      hci_spec::kLEReadAdvertisingChannelTxPower,
      pw::bluetooth::emboss::StatusCode::HARDWARE_FAILURE);

  std::optional<Result<AdvertisementId>> last_status;
  auto result_cb = [&](Result<AdvertisementId> status) {
    last_status = status;
  };

  this->advertiser()->StartAdvertising(kPublicAddress,
                                       ad,
                                       scan_data,
                                       options,
                                       /*connect_callback=*/nullptr,
                                       result_cb);
  this->RunUntilIdle();

  ASSERT_TRUE(last_status.has_value());
  EXPECT_TRUE(last_status->is_error());
  EXPECT_TRUE(last_status->error_value().is(
      pw::bluetooth::emboss::StatusCode::HARDWARE_FAILURE));
  EXPECT_FALSE(this->advertiser()->IsAdvertising());
}

// Ensure that when starting advertising with TX power level included, if the
// controller returns a malformed (too small) response for the read TX power
// level command, advertising fails to start and a malformed packet error is
// propagated without crashing the system.
TEST_F(AndroidLowEnergyMultipleAdvertisingTest,
       StartAdvertisingFailureTxPowerReadMalformed) {
  this->test_device()->set_num_supported_advertising_sets(1);

  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*init_extended_pdu=*/false,
                             /*init_anonymous=*/false,
                             /*init_include_tx_power_level=*/true);

  bool paused = false;
  this->test_device()->pause_responses_for_opcode(
      hci_spec::kLEReadAdvertisingChannelTxPower,
      [&](fit::closure /*resume*/) { paused = true; });

  std::optional<Result<AdvertisementId>> last_status;
  auto result_cb = [&](Result<AdvertisementId> status) {
    last_status = status;
  };

  this->advertiser()->StartAdvertising(kPublicAddress,
                                       ad,
                                       scan_data,
                                       options,
                                       /*connect_callback=*/nullptr,
                                       result_cb);
  this->RunUntilIdle();
  ASSERT_TRUE(paused);

  // Send a malformed response (SimpleCommandCompleteEvent with SUCCESS).
  // This is too small for LEReadAdvertisingChannelTxPowerCommandCompleteEvent.
  auto packet = hci::EventPacket::New<
      pw::bluetooth::emboss::SimpleCommandCompleteEventWriter>(
      hci_spec::kCommandCompleteEventCode);
  auto view = packet.view_t();
  view.status().Write(pw::bluetooth::emboss::StatusCode::SUCCESS);

  auto header =
      packet.view<pw::bluetooth::emboss::CommandCompleteEventWriter>();
  header.num_hci_command_packets().Write(1);
  header.command_opcode().Write(static_cast<pw::bluetooth::emboss::OpCode>(
      hci_spec::kLEReadAdvertisingChannelTxPower));

  this->test_device()->SendCommandChannelPacket(packet.data());
  this->RunUntilIdle();

  ASSERT_TRUE(last_status.has_value());
  EXPECT_TRUE(last_status->is_error());
  EXPECT_TRUE(last_status->error_value().is(HostError::kPacketMalformed));
  EXPECT_FALSE(this->advertiser()->IsAdvertising());

  this->test_device()->clear_pause_listener_for_opcode(
      hci_spec::kLEReadAdvertisingChannelTxPower);
}

}  // namespace
}  // namespace bt::hci
