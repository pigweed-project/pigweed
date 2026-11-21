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

#include "pw_bluetooth_sapphire/internal/host/hci/extended_low_energy_advertiser.h"

#include "pw_bluetooth_sapphire/internal/host/testing/controller_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_controller.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"

namespace bt::hci {
namespace {

using bt::testing::FakeController;
using TestingBase = bt::testing::FakeDispatcherControllerTest<FakeController>;
using AdvertisingOptions = LowEnergyAdvertiser::AdvertisingOptions;
using LEAdvertisingState = FakeController::LEAdvertisingState;

constexpr AdvertisingIntervalRange kTestInterval(
    hci_spec::kLEAdvertisingIntervalMin, hci_spec::kLEAdvertisingIntervalMax);

const DeviceAddress kPublicAddress(DeviceAddress::Type::kLEPublic, {1});
const DeviceAddress kRandomAddress(DeviceAddress::Type::kLERandom, {2});
const bool kExtendedPdu = true;

class ExtendedLowEnergyAdvertiserTest : public TestingBase {
 public:
  ExtendedLowEnergyAdvertiserTest() = default;
  ~ExtendedLowEnergyAdvertiserTest() override = default;

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
    this->test_device()->set_maximum_advertising_data_length(
        hci_spec::kMaxLEExtendedAdvertisingDataLength);

    advertiser_ = std::make_unique<ExtendedLowEnergyAdvertiser>(
        transport()->GetWeakPtr(),
        dispatcher(),
        hci_spec::kMaxLEExtendedAdvertisingDataLength);
  }

  void TearDown() override {
    advertiser_ = nullptr;
    TestingBase::TearDown();
  }

  ExtendedLowEnergyAdvertiser* advertiser() const { return advertiser_.get(); }

  void DestroyAdvertiser() { advertiser_.reset(); }

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

  AdvertisingData GetExampleDataMultiplePDUs() {
    AdvertisingData result;

    const uint16_t num_pdus = 2;
    for (uint16_t i = 0; i < num_pdus; i++) {
      SetServiceData(result, i, kMaxEncodedServiceDataLength);
    }

    return result;
  }

  AdvertisingData GetExampleDataTooLarge() {
    AdvertisingData result;

    size_t num_pdus = hci_spec::kMaxLEExtendedAdvertisingDataLength /
                          kMaxEncodedServiceDataLength +
                      1;

    for (uint16_t i = 0; i < num_pdus; i++) {
      SetServiceData(result, i, kMaxEncodedServiceDataLength);
    }

    return result;
  }

  std::optional<Result<AdvertisementId>> GetLastStatus() {
    if (!last_status_) {
      return std::nullopt;
    }

    return std::exchange(last_status_, std::nullopt).value();
  }

 private:
  void SetServiceData(AdvertisingData& result, uint16_t id, uint8_t size) {
    // subtract 2 to allow for uuid size and service data tag
    std::string data(size - 2, 'a');

    auto service_uuid = UUID(id);
    EXPECT_TRUE(result.AddServiceUuid(service_uuid));

    DynamicByteBuffer service_data(data);
    EXPECT_TRUE(result.SetServiceData(service_uuid, service_data.view()));
  }

  std::unique_ptr<ExtendedLowEnergyAdvertiser> advertiser_;
  std::optional<Result<AdvertisementId>> last_status_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(ExtendedLowEnergyAdvertiserTest);
};

// Ensure tx power level is included in advertising data and scan response data.
// We check for hci_spec::kLEAdvertisingTxPowerMax simply because that's the
// value used in FakeController when handling HCI LE Set Extended Advertising
// Parameters.
TEST_F(ExtendedLowEnergyAdvertiserTest, TxPowerLevelRetrieved) {
  AdvertisingData ad = GetExampleData();
  AdvertisingData scan_data;
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             kExtendedPdu,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/true);

  std::unique_ptr<LowEnergyConnection> link;
  auto conn_cb = [&link](auto, auto cb_link) { link = std::move(cb_link); };

  this->advertiser()->StartAdvertising(kPublicAddress,
                                       ad,
                                       scan_data,
                                       options,
                                       conn_cb,
                                       this->MakeExpectSuccessCallback());
  RunUntilIdle();
  std::optional<hci_spec::AdvertisingHandle> handle =
      this->advertiser()->LastUsedHandleForTesting();
  ASSERT_TRUE(handle);
  std::optional<Result<AdvertisementId>> status = GetLastStatus();
  ASSERT_TRUE(status.has_value());
  ASSERT_TRUE(status->is_ok());
  AdvertisementId advertisement_id = status->value();
  EXPECT_EQ(1u, this->advertiser()->NumAdvertisements());
  EXPECT_TRUE(this->advertiser()->IsAdvertising());
  EXPECT_TRUE(this->advertiser()->IsAdvertising(advertisement_id));

  const LEAdvertisingState& st =
      this->test_device()->extended_advertising_state(handle.value());

  AdvertisingData::ParseResult actual_ad =
      AdvertisingData::FromBytes(st.advertised_view());
  ASSERT_EQ(fit::ok(), actual_ad);
  EXPECT_EQ(hci_spec::kLEAdvertisingTxPowerMax, actual_ad.value().tx_power());
}

// Ensure we can use extended advertising PDUs and advertise a connectable
// advertisement with some advertising data.
TEST_F(ExtendedLowEnergyAdvertiserTest, ExtendedPDUsConnectable) {
  AdvertisingData ad = GetExampleData();
  AdvertisingData scan_data;
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             kExtendedPdu,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/false);

  this->advertiser()->StartAdvertising(
      kPublicAddress,
      ad,
      scan_data,
      options,
      [](auto, auto) {},
      this->MakeExpectSuccessCallback());
  RunUntilIdle();

  std::optional<hci_spec::AdvertisingHandle> handle =
      this->advertiser()->LastUsedHandleForTesting();
  ASSERT_TRUE(handle);
  std::optional<Result<AdvertisementId>> status = GetLastStatus();
  ASSERT_TRUE(status.has_value());
  ASSERT_TRUE(status->is_ok());
  AdvertisementId advertisement_id = status->value();
  EXPECT_EQ(1u, this->advertiser()->NumAdvertisements());
  EXPECT_TRUE(this->advertiser()->IsAdvertising());
  EXPECT_TRUE(this->advertiser()->IsAdvertising(advertisement_id));

  const LEAdvertisingState& st =
      this->test_device()->extended_advertising_state(handle.value());
  ASSERT_TRUE(st.properties.connectable);
  ASSERT_FALSE(st.properties.scannable);
  ASSERT_FALSE(st.properties.directed);
  ASSERT_FALSE(st.properties.high_duty_cycle_directed_connectable);
  ASSERT_FALSE(st.properties.use_legacy_pdus);
  ASSERT_FALSE(st.properties.anonymous_advertising);
  ASSERT_FALSE(st.properties.include_tx_power);

  AdvertisingData::ParseResult actual_ad =
      AdvertisingData::FromBytes(st.advertised_view());
  ASSERT_EQ(fit::ok(), actual_ad);
}

// Ensure we can use extended advertising PDUs and advertise a scannable
// advertisement with some scan response data.
TEST_F(ExtendedLowEnergyAdvertiserTest, ExtendedPDUsScannable) {
  AdvertisingData ad;
  AdvertisingData scan_data = GetExampleData();
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             kExtendedPdu,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/false);

  this->advertiser()->StartAdvertising(kPublicAddress,
                                       ad,
                                       scan_data,
                                       options,
                                       nullptr,
                                       this->MakeExpectSuccessCallback());
  RunUntilIdle();

  std::optional<hci_spec::AdvertisingHandle> handle =
      this->advertiser()->LastUsedHandleForTesting();
  ASSERT_TRUE(handle);
  std::optional<Result<AdvertisementId>> status = GetLastStatus();
  ASSERT_TRUE(status.has_value());
  ASSERT_TRUE(status->is_ok());
  AdvertisementId advertisement_id = status->value();
  EXPECT_EQ(1u, this->advertiser()->NumAdvertisements());
  EXPECT_TRUE(this->advertiser()->IsAdvertising());
  EXPECT_TRUE(this->advertiser()->IsAdvertising(advertisement_id));

  const LEAdvertisingState& st =
      this->test_device()->extended_advertising_state(handle.value());
  ASSERT_FALSE(st.properties.connectable);
  ASSERT_TRUE(st.properties.scannable);
  ASSERT_FALSE(st.properties.directed);
  ASSERT_FALSE(st.properties.high_duty_cycle_directed_connectable);
  ASSERT_FALSE(st.properties.use_legacy_pdus);
  ASSERT_FALSE(st.properties.anonymous_advertising);
  ASSERT_FALSE(st.properties.include_tx_power);

  AdvertisingData::ParseResult actual_scan_rsp =
      AdvertisingData::FromBytes(st.scan_rsp_view());
  ASSERT_EQ(fit::ok(), actual_scan_rsp);
}

// Core Spec Version 5.4, Volume 5, Part E, Section 7.8.53: If extended
// advertising PDU types are being used then the advertisement shall not be both
// connectable and scannable.
TEST_F(ExtendedLowEnergyAdvertiserTest, ExtendedPDUsConnectableAndScannable) {
  AdvertisingData ad;
  AdvertisingData scan_data = GetExampleData();
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             kExtendedPdu,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/false);

  this->advertiser()->StartAdvertising(
      kPublicAddress,
      ad,
      scan_data,
      options,
      [](auto, auto) {},
      this->MakeExpectErrorCallback());
  RunUntilIdle();

  EXPECT_FALSE(this->advertiser()->IsAdvertising());
}

// Ensure we can send fragmented advertising data to the Controller across
// multiple HCI packets
TEST_F(ExtendedLowEnergyAdvertiserTest, AdvertisingDataFragmented) {
  AdvertisingData ad = GetExampleDataMultiplePDUs();
  AdvertisingData scan_data;
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             kExtendedPdu,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/false);

  this->advertiser()->StartAdvertising(
      kPublicAddress,
      ad,
      scan_data,
      options,
      [](auto, auto) {},
      this->MakeExpectSuccessCallback());
  RunUntilIdle();

  std::optional<hci_spec::AdvertisingHandle> handle =
      this->advertiser()->LastUsedHandleForTesting();
  ASSERT_TRUE(handle);
  std::optional<Result<AdvertisementId>> status = GetLastStatus();
  ASSERT_TRUE(status.has_value());
  ASSERT_TRUE(status->is_ok());
  AdvertisementId advertisement_id = status->value();
  EXPECT_EQ(1u, this->advertiser()->NumAdvertisements());
  EXPECT_TRUE(this->advertiser()->IsAdvertising());
  EXPECT_TRUE(this->advertiser()->IsAdvertising(advertisement_id));

  const LEAdvertisingState& st =
      this->test_device()->extended_advertising_state(handle.value());
  ASSERT_EQ(ad.CalculateBlockSize(/*include_flags=*/true), st.data_length);

  AdvertisingData::ParseResult actual_ad =
      AdvertisingData::FromBytes(st.advertised_view());
  ASSERT_EQ(fit::ok(), actual_ad);

  size_t block_size = ad.CalculateBlockSize(/*include_flags=*/true);
  DynamicByteBuffer buffer(block_size);
  ad.WriteBlock(&buffer, options.flags);
  ASSERT_EQ(buffer.ToString(), st.advertised_view().ToString());
}

// Ensure we can send fragmented scan response data to the Controller across
// multiple HCI packets
TEST_F(ExtendedLowEnergyAdvertiserTest, ScanResponseDataFragmented) {
  AdvertisingData ad;
  AdvertisingData scan_data = GetExampleDataMultiplePDUs();
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             kExtendedPdu,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/false);

  this->advertiser()->StartAdvertising(kPublicAddress,
                                       ad,
                                       scan_data,
                                       options,
                                       nullptr,
                                       this->MakeExpectSuccessCallback());
  RunUntilIdle();

  std::optional<hci_spec::AdvertisingHandle> handle =
      this->advertiser()->LastUsedHandleForTesting();
  ASSERT_TRUE(handle);
  std::optional<Result<AdvertisementId>> status = GetLastStatus();
  ASSERT_TRUE(status.has_value());
  ASSERT_TRUE(status->is_ok());
  AdvertisementId advertisement_id = status->value();
  EXPECT_EQ(1u, this->advertiser()->NumAdvertisements());
  EXPECT_TRUE(this->advertiser()->IsAdvertising());
  EXPECT_TRUE(this->advertiser()->IsAdvertising(advertisement_id));

  const LEAdvertisingState& st =
      this->test_device()->extended_advertising_state(handle.value());
  ASSERT_EQ(scan_data.CalculateBlockSize(), st.scan_rsp_length);

  AdvertisingData::ParseResult actual_scan_rsp =
      AdvertisingData::FromBytes(st.scan_rsp_view());
  ASSERT_EQ(fit::ok(), actual_scan_rsp);

  size_t block_size = scan_data.CalculateBlockSize();
  DynamicByteBuffer buffer(block_size);
  scan_data.WriteBlock(&buffer, std::nullopt);
  ASSERT_EQ(buffer.ToString(), st.scan_rsp_view().ToString());
}

// Ensure that we aren't able to advertise if we are sending advertising data
// larger than what the spec allows.
TEST_F(ExtendedLowEnergyAdvertiserTest, AdvertisingDataTooLarge) {
  AdvertisingData ad = GetExampleDataTooLarge();
  AdvertisingData scan_data;
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             kExtendedPdu,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/false);

  this->advertiser()->StartAdvertising(
      kPublicAddress,
      ad,
      scan_data,
      options,
      [](auto, auto) {},
      this->MakeExpectErrorCallback());
  RunUntilIdle();
  EXPECT_FALSE(this->advertiser()->IsAdvertising());
}

// Ensure that we aren't able to advertise if we are sending scan response data
// larger than what the spec allows.
TEST_F(ExtendedLowEnergyAdvertiserTest, ScanResposneDataTooLarge) {
  AdvertisingData ad;
  AdvertisingData scan_data = GetExampleDataTooLarge();
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             kExtendedPdu,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/false);

  this->advertiser()->StartAdvertising(kPublicAddress,
                                       ad,
                                       scan_data,
                                       options,
                                       nullptr,
                                       this->MakeExpectErrorCallback());
  RunUntilIdle();
  EXPECT_FALSE(this->advertiser()->IsAdvertising());
}

// Ensure that we aren't able to advertise if we are sending advertising data
// larger than what is currently configured by the Controller.
TEST_F(ExtendedLowEnergyAdvertiserTest, AdvertisingDataLargerThanConfigured) {
  test_device()->set_maximum_advertising_data_length(
      hci_spec::kMaxLEAdvertisingDataLength);

  // Use our own local advertiser. Just for this test, we don't want to modify
  // the entire test API in this file to be able to reset the advertiser.
  std::unique_ptr<ExtendedLowEnergyAdvertiser> advertiser =
      std::make_unique<ExtendedLowEnergyAdvertiser>(
          transport()->GetWeakPtr(),
          dispatcher(),
          hci_spec::kMaxLEAdvertisingDataLength);

  AdvertisingData ad = GetExampleDataMultiplePDUs();
  AdvertisingData scan_data;
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             kExtendedPdu,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/false);

  advertiser->StartAdvertising(
      kPublicAddress,
      ad,
      scan_data,
      options,
      [](auto, auto) {},
      this->MakeExpectErrorCallback());
  RunUntilIdle();
  EXPECT_FALSE(this->advertiser()->IsAdvertising());
}

// Ensure that we aren't able to advertise if we are sending data larger than
// what is currently configured by the Controller.
TEST_F(ExtendedLowEnergyAdvertiserTest, ScanResponseDataLargerThanConfigured) {
  test_device()->set_maximum_advertising_data_length(
      hci_spec::kMaxLEAdvertisingDataLength);

  // Use our own local advertiser. Just for this test, we don't want to modify
  // the entire test API in this file to be able to reset the advertiser.
  std::unique_ptr<ExtendedLowEnergyAdvertiser> advertiser =
      std::make_unique<ExtendedLowEnergyAdvertiser>(
          transport()->GetWeakPtr(),
          dispatcher(),
          hci_spec::kMaxLEAdvertisingDataLength);

  AdvertisingData ad;
  AdvertisingData scan_data = GetExampleDataMultiplePDUs();
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             kExtendedPdu,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/false);

  advertiser->StartAdvertising(kPublicAddress,
                               ad,
                               scan_data,
                               options,
                               nullptr,
                               this->MakeExpectErrorCallback());
  RunUntilIdle();
  EXPECT_FALSE(this->advertiser()->IsAdvertising());
}

TEST_F(ExtendedLowEnergyAdvertiserTest,
       DestroyAdvertiserInIncomingConnectionCallback) {
  constexpr hci_spec::ConnectionHandle kConnectionHandle = 0x0001;

  AdvertisingData ad = GetExampleData();
  AdvertisingData scan_data;
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             kExtendedPdu,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/true);

  bool connected = false;
  std::unique_ptr<LowEnergyConnection> received_link;
  auto conn_cb = [&](AdvertisementId,
                     std::unique_ptr<LowEnergyConnection> link) {
    connected = true;
    received_link = std::move(link);
    this->DestroyAdvertiser();
  };

  std::optional<Result<AdvertisementId>> start_result;
  this->advertiser()->StartAdvertising(
      kPublicAddress,
      ad,
      scan_data,
      options,
      std::move(conn_cb),
      [&](Result<AdvertisementId> result) { start_result = result; });
  RunUntilIdle();
  ASSERT_TRUE(start_result.has_value());
  ASSERT_TRUE(start_result->is_ok());

  std::optional<hci_spec::AdvertisingHandle> handle =
      this->advertiser()->LastUsedHandleForTesting();
  ASSERT_TRUE(handle.has_value());

  this->advertiser()->OnIncomingConnection(
      kConnectionHandle,
      pw::bluetooth::emboss::ConnectionRole::PERIPHERAL,
      kRandomAddress,
      hci_spec::LEConnectionParameters());
  test_device()->SendLEAdvertisingSetTerminatedEvent(kConnectionHandle,
                                                     handle.value());
  RunUntilIdle();

  EXPECT_TRUE(connected);
  EXPECT_EQ(nullptr, this->advertiser());
  ASSERT_NE(nullptr, received_link);
}

TEST_F(ExtendedLowEnergyAdvertiserTest,
       IncomingConnectionOnFirstSetDoesNotClobberInFlightSecondSetTxPower) {
  constexpr hci_spec::ConnectionHandle kConnectionHandle = 0x0001;
  test_device()->set_num_supported_advertising_sets(2);
  AdvertisingData ad = GetExampleData();
  AdvertisingData scan_data;
  AdvertisingOptions set1_options(kTestInterval,
                                  kDefaultNoAdvFlags,
                                  kExtendedPdu,
                                  /*anonymous=*/false,
                                  /*include_tx_power_level=*/false);
  AdvertisingOptions set2_options(kTestInterval,
                                  kDefaultNoAdvFlags,
                                  kExtendedPdu,
                                  /*anonymous=*/false,
                                  /*include_tx_power_level=*/true);

  std::unique_ptr<LowEnergyConnection> set1_link;
  this->advertiser()->StartAdvertising(
      kPublicAddress,
      ad,
      scan_data,
      set1_options,
      [&](AdvertisementId, std::unique_ptr<LowEnergyConnection> link) {
        set1_link = std::move(link);
      },
      this->MakeExpectSuccessCallback());
  RunUntilIdle();
  std::optional<hci_spec::AdvertisingHandle> set1_handle =
      this->advertiser()->LastUsedHandleForTesting();
  ASSERT_TRUE(set1_handle.has_value());

  // Begin starting Set 2 with include_tx_power_level=true so
  // LE_Set_Extended_Advertising_Parameters is in flight while Set 1 receives an
  // incoming connection and LE_Advertising_Set_Terminated event.
  fit::closure resume_set2_params;
  test_device()->pause_responses_for_opcode(
      hci_spec::kLESetExtendedAdvertisingParameters,
      [&](fit::closure resume) { resume_set2_params = std::move(resume); });
  std::optional<Result<AdvertisementId>> set2_result;
  this->advertiser()->StartAdvertising(
      kRandomAddress,
      ad,
      scan_data,
      set2_options,
      nullptr,
      [&](Result<AdvertisementId> result) { set2_result = result; });
  std::optional<hci_spec::AdvertisingHandle> set2_handle =
      this->advertiser()->LastUsedHandleForTesting();
  ASSERT_TRUE(set2_handle.has_value());
  EXPECT_NE(*set1_handle, *set2_handle);

  this->advertiser()->OnIncomingConnection(
      kConnectionHandle,
      pw::bluetooth::emboss::ConnectionRole::PERIPHERAL,
      kRandomAddress,
      hci_spec::LEConnectionParameters());
  test_device()->SendLEAdvertisingSetTerminatedEvent(kConnectionHandle,
                                                     set1_handle.value());
  RunUntilIdle();

  ASSERT_NE(nullptr, set1_link);
  ASSERT_FALSE(set2_result.has_value());
  ASSERT_TRUE(resume_set2_params);
  resume_set2_params();
  RunUntilIdle();

  ASSERT_TRUE(set2_result.has_value());
  ASSERT_TRUE(set2_result->is_ok());
  const LEAdvertisingState& st =
      test_device()->extended_advertising_state(set2_handle.value());
  AdvertisingData::ParseResult actual_ad =
      AdvertisingData::FromBytes(st.advertised_view());
  ASSERT_EQ(fit::ok(), actual_ad);
  EXPECT_EQ(hci_spec::kLEAdvertisingTxPowerMax, actual_ad.value().tx_power());
}

}  // namespace
}  // namespace bt::hci
