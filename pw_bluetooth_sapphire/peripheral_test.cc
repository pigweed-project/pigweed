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

#include "pw_bluetooth_sapphire/peripheral.h"

#include "pw_async/fake_dispatcher.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/func_task.h"
#include "pw_bluetooth_sapphire/internal/host/gap/fake_adapter.h"
#include "pw_unit_test/framework.h"

using Peripheral2 = pw::bluetooth::low_energy::Peripheral2;
using AdvertisedPeripheral2 = pw::bluetooth::low_energy::AdvertisedPeripheral2;
using ManufacturerData = pw::bluetooth::low_energy::ManufacturerData;
using AdvertiseError = pw::bluetooth::low_energy::Peripheral2::AdvertiseError;

template <typename T>
class ReceiverTask final : public pw::async2::Task {
 public:
  ReceiverTask(pw::async2::OptionalValueFuture<T> future)
      : future_(std::move(future)) {}

  pw::async2::Poll<> DoPend(pw::async2::Context& cx) override {
    pw::async2::Poll<std::optional<T>> pend = future_.Pend(cx);
    if (pend.IsPending()) {
      return pw::async2::Pending();
    }
    result_ = std::move(pend.value());
    return pw::async2::Ready();
  }

  std::optional<T>& result() { return result_; }

 private:
  pw::async2::OptionalValueFuture<T> future_;
  std::optional<T> result_;
};

class PeripheralTest : public ::testing::Test {
 public:
  void SetUp() override {}

  void TearDown() override {}

  // Returns nullopt if ValueFuture received no result or a nullopt result.
  std::optional<Peripheral2::AdvertiseResult> Advertise(
      Peripheral2::AdvertisingParameters& parameters) {
    pw::async2::OptionalValueFuture<Peripheral2::AdvertiseResult> future =
        peripheral().Advertise(parameters);

    ReceiverTask<Peripheral2::AdvertiseResult> task(std::move(future));
    dispatcher2().Post(task);
    EXPECT_FALSE(task.result().has_value());

    dispatcher().RunUntilIdle();
    dispatcher2().RunToCompletion();
    if (!task.result().has_value()) {
      return std::nullopt;
    }
    return std::move(task.result().value());
  }

  AdvertisedPeripheral2::Ptr AdvertiseExpectSuccess(
      Peripheral2::AdvertisingParameters& parameters) {
    std::optional<Peripheral2::AdvertiseResult> result = Advertise(parameters);
    if (!result.has_value()) {
      ADD_FAILURE();
      return nullptr;
    }
    if (!result.value().has_value()) {
      ADD_FAILURE();
      return nullptr;
    }
    return std::move(result.value().value());
  }

  pw::bluetooth_sapphire::Peripheral& peripheral() { return peripheral_; }

  bt::gap::testing::FakeAdapter& adapter() { return adapter_; }

  pw::async::test::FakeDispatcher& dispatcher() { return async_dispatcher_; }
  pw::async2::RunnableDispatcher& dispatcher2() { return async2_dispatcher_; }

 private:
  pw::async::test::FakeDispatcher async_dispatcher_;
  pw::async2::DispatcherForTest async2_dispatcher_;
  bt::gap::testing::FakeAdapter adapter_{async_dispatcher_};
  pw::bluetooth_sapphire::Peripheral peripheral_{adapter_.AsWeakPtr(),
                                                 async_dispatcher_};
};

TEST_F(PeripheralTest,
       StartAdvertisingWithNameAndDestroyAdvertisedPeripheralStopsAdvertising) {
  Peripheral2::AdvertisingParameters parameters;
  parameters.data.name = "pigweed";

  AdvertisedPeripheral2::Ptr advertised_peripheral =
      AdvertiseExpectSuccess(parameters);

  ASSERT_EQ(adapter().fake_le()->registered_advertisements().size(), 1u);
  auto& advertisement =
      adapter().fake_le()->registered_advertisements().begin()->second;
  EXPECT_EQ(advertisement.data.local_name()->name, parameters.data.name);
  EXPECT_EQ(advertisement.data.appearance(),
            static_cast<uint16_t>(pw::bluetooth::Appearance::kUnknown));
  EXPECT_FALSE(advertisement.extended_pdu);
  EXPECT_FALSE(advertisement.include_tx_power_level);
  EXPECT_FALSE(advertisement.connectable);
  EXPECT_FALSE(advertisement.anonymous);

  advertised_peripheral.reset();
  dispatcher().RunUntilIdle();
  ASSERT_EQ(adapter().fake_le()->registered_advertisements().size(), 0u);
}

TEST_F(PeripheralTest, StartAdvertisingWithTooLongName) {
  std::string name(300, 'A');
  Peripheral2::AdvertisingParameters parameters;
  parameters.data.name = name;
  std::optional<Peripheral2::AdvertiseResult> result = Advertise(parameters);
  ASSERT_TRUE(result.has_value());
  ASSERT_FALSE(result.value().has_value());
  EXPECT_EQ(result.value().error(),
            pw::bluetooth_sapphire::Peripheral::AdvertiseError::
                kAdvertisingDataTooLong);
}

TEST_F(PeripheralTest, StartAdvertisingWithServiceData) {
  const uint16_t uuid_0 = 42, uuid_1 = 43;
  pw::bluetooth::low_energy::ServiceData service_data_0;
  service_data_0.uuid = pw::bluetooth::Uuid(uuid_0);
  std::array<std::byte, 3> service_data_0_data = {
      std::byte{0x00}, std::byte{0x01}, std::byte{0x02}};
  service_data_0.data = pw::span(service_data_0_data);

  pw::bluetooth::low_energy::ServiceData service_data_1;
  service_data_1.uuid = pw::bluetooth::Uuid(uuid_1);
  std::array<std::byte, 3> service_data_1_data = {
      std::byte{0x10}, std::byte{0x11}, std::byte{0x12}};
  service_data_1.data = pw::span(service_data_1_data);

  std::array<pw::bluetooth::low_energy::ServiceData, 2> service_data = {
      service_data_0, service_data_1};
  Peripheral2::AdvertisingParameters parameters;
  parameters.data.service_data = service_data;

  AdvertisedPeripheral2::Ptr advertised_peripheral =
      AdvertiseExpectSuccess(parameters);

  ASSERT_EQ(adapter().fake_le()->registered_advertisements().size(), 1u);
  auto& advertisement =
      adapter().fake_le()->registered_advertisements().begin()->second;
  EXPECT_EQ(advertisement.data.service_data(bt::UUID(uuid_0)),
            bt::BufferView(service_data_0.data));
  EXPECT_EQ(advertisement.data.service_data(bt::UUID(uuid_1)),
            bt::BufferView(service_data_1.data));
}

TEST_F(PeripheralTest, StartAdvertisingWithServiceUuids) {
  const uint16_t uuid_0 = 42, uuid_1 = 43;
  std::array<pw::bluetooth::Uuid, 2> service_uuids = {
      pw::bluetooth::Uuid(uuid_0), pw::bluetooth::Uuid(uuid_1)};
  std::unordered_set<bt::UUID> expected_uuids{bt::UUID(uuid_0),
                                              bt::UUID(uuid_1)};
  Peripheral2::AdvertisingParameters parameters;
  parameters.data.service_uuids = service_uuids;

  AdvertisedPeripheral2::Ptr advertised_peripheral =
      AdvertiseExpectSuccess(parameters);

  ASSERT_EQ(adapter().fake_le()->registered_advertisements().size(), 1u);
  auto& advertisement =
      adapter().fake_le()->registered_advertisements().begin()->second;
  EXPECT_EQ(advertisement.data.service_uuids(), expected_uuids);
}

TEST_F(PeripheralTest, StartAdvertisingWithManufacturerData) {
  std::array<std::byte, 3> data_0 = {
      std::byte{0x00}, std::byte{0x01}, std::byte{0x02}};
  std::array<std::byte, 3> data_1 = {
      std::byte{0x03}, std::byte{0x04}, std::byte{0x05}};
  std::array<ManufacturerData, 2> manufacturer_data{
      ManufacturerData{.company_id = 0, .data = data_0},
      ManufacturerData{.company_id = 1, .data = data_1}};
  Peripheral2::AdvertisingParameters parameters;
  parameters.data.manufacturer_data = manufacturer_data;

  AdvertisedPeripheral2::Ptr advertised_peripheral =
      AdvertiseExpectSuccess(parameters);

  ASSERT_EQ(adapter().fake_le()->registered_advertisements().size(), 1u);
  auto& advertisement =
      adapter().fake_le()->registered_advertisements().begin()->second;
  EXPECT_EQ(advertisement.data.manufacturer_data(0), bt::BufferView(data_0));
  EXPECT_EQ(advertisement.data.manufacturer_data(1), bt::BufferView(data_1));
}

TEST_F(PeripheralTest, StartAdvertisingWithUris) {
  std::string uri_0("https://abc.xyz");
  std::string uri_1("https://pigweed.dev");
  std::array<std::string_view, 2> uris = {uri_0, uri_1};
  std::unordered_set<std::string> uri_set = {uri_0, uri_1};
  Peripheral2::AdvertisingParameters parameters;
  parameters.data.uris = uris;

  AdvertisedPeripheral2::Ptr advertised_peripheral =
      AdvertiseExpectSuccess(parameters);

  ASSERT_EQ(adapter().fake_le()->registered_advertisements().size(), 1u);
  auto& advertisement =
      adapter().fake_le()->registered_advertisements().begin()->second;
  EXPECT_EQ(advertisement.data.uris(), uri_set);
}

TEST_F(PeripheralTest, StartAdvertisingWithPublicAddressType) {
  Peripheral2::AdvertisingParameters parameters;
  parameters.address_type = pw::bluetooth::Address::Type::kPublic;

  AdvertisedPeripheral2::Ptr advertised_peripheral =
      AdvertiseExpectSuccess(parameters);

  ASSERT_EQ(adapter().fake_le()->registered_advertisements().size(), 1u);
  auto& advertisement =
      adapter().fake_le()->registered_advertisements().begin()->second;
  EXPECT_EQ(advertisement.addr_type, bt::DeviceAddress::Type::kLEPublic);
}

TEST_F(PeripheralTest, StartAdvertisingWithRandomAddressType) {
  adapter().fake_le()->EnablePrivacy(true);

  Peripheral2::AdvertisingParameters parameters;
  parameters.address_type =
      pw::bluetooth::Address::Type::kRandomResolvablePrivate;

  AdvertisedPeripheral2::Ptr advertised_peripheral =
      AdvertiseExpectSuccess(parameters);

  ASSERT_EQ(adapter().fake_le()->registered_advertisements().size(), 1u);
  auto& advertisement =
      adapter().fake_le()->registered_advertisements().begin()->second;
  EXPECT_EQ(advertisement.addr_type, bt::DeviceAddress::Type::kLERandom);
}

TEST_F(PeripheralTest, StartAdvertisingWithLegacyProcedureWithScanResponse) {
  pw::bluetooth::low_energy::AdvertisingData scan_rsp;
  scan_rsp.name = "robot";
  Peripheral2::AdvertisingParameters parameters;
  parameters.procedure = Peripheral2::LegacyAdvertising{
      .scan_response = std::move(scan_rsp), .connection_options = std::nullopt};

  AdvertisedPeripheral2::Ptr advertised_peripheral =
      AdvertiseExpectSuccess(parameters);

  ASSERT_EQ(adapter().fake_le()->registered_advertisements().size(), 1u);
  auto& advertisement =
      adapter().fake_le()->registered_advertisements().begin()->second;
  EXPECT_EQ(advertisement.scan_response.local_name()->name, "robot");
}

TEST_F(PeripheralTest,
       StartAdvertisingWithLegacyProcedureWithConnectionOptionsNonBondable) {
  Peripheral2::AdvertisingParameters parameters;
  Peripheral2::ConnectionOptions connection_options{
      .bondable_mode = false,
      .service_filter = std::nullopt,
      .parameters = std::nullopt,
      .att_mtu = std::nullopt};
  parameters.procedure = Peripheral2::LegacyAdvertising{
      .scan_response = std::nullopt, .connection_options = connection_options};

  AdvertisedPeripheral2::Ptr advertised_peripheral =
      AdvertiseExpectSuccess(parameters);

  ASSERT_EQ(adapter().fake_le()->registered_advertisements().size(), 1u);
  auto& advertisement =
      adapter().fake_le()->registered_advertisements().begin()->second;
  ASSERT_TRUE(advertisement.connectable.has_value());
  EXPECT_EQ(advertisement.connectable->bondable_mode,
            bt::sm::BondableMode::NonBondable);
}

TEST_F(PeripheralTest,
       StartAdvertisingWithLegacyProcedureWithConnectionOptionsBondable) {
  Peripheral2::AdvertisingParameters parameters;
  Peripheral2::ConnectionOptions connection_options{
      .bondable_mode = true,
      .service_filter = std::nullopt,
      .parameters = std::nullopt,
      .att_mtu = std::nullopt};
  parameters.procedure = Peripheral2::LegacyAdvertising{
      .scan_response = std::nullopt, .connection_options = connection_options};

  AdvertisedPeripheral2::Ptr advertised_peripheral =
      AdvertiseExpectSuccess(parameters);

  ASSERT_EQ(adapter().fake_le()->registered_advertisements().size(), 1u);
  auto& advertisement =
      adapter().fake_le()->registered_advertisements().begin()->second;
  ASSERT_TRUE(advertisement.connectable.has_value());
  EXPECT_EQ(advertisement.connectable->bondable_mode,
            bt::sm::BondableMode::Bondable);
}

TEST_F(PeripheralTest, StartAdvertisingAnonymous) {
  Peripheral2::AdvertisingParameters parameters;
  parameters.procedure = Peripheral2::ExtendedAdvertising{
      .configuration = pw::bluetooth::low_energy::Peripheral2::
          ExtendedAdvertising::Anonymous(),
      .tx_power = std::nullopt,
      .primary_phy = pw::bluetooth::low_energy::Phy::k1Megabit,
      .secondary_phy = pw::bluetooth::low_energy::Phy::k1Megabit};

  AdvertisedPeripheral2::Ptr advertised_peripheral =
      AdvertiseExpectSuccess(parameters);

  ASSERT_EQ(adapter().fake_le()->registered_advertisements().size(), 1u);
  auto& advertisement =
      adapter().fake_le()->registered_advertisements().begin()->second;
  EXPECT_TRUE(advertisement.anonymous);
}

TEST_F(PeripheralTest, StartAdvertisingWithExtendedProcedureWithScanResponse) {
  Peripheral2::ScanResponse scan_rsp;
  scan_rsp.name = "robot";
  Peripheral2::AdvertisingParameters parameters;
  parameters.procedure = Peripheral2::ExtendedAdvertising{
      .configuration = scan_rsp,
      .tx_power = std::nullopt,
      .primary_phy = pw::bluetooth::low_energy::Phy::k1Megabit,
      .secondary_phy = pw::bluetooth::low_energy::Phy::k1Megabit};

  AdvertisedPeripheral2::Ptr advertised_peripheral =
      AdvertiseExpectSuccess(parameters);

  ASSERT_EQ(adapter().fake_le()->registered_advertisements().size(), 1u);
  auto& advertisement =
      adapter().fake_le()->registered_advertisements().begin()->second;
  EXPECT_EQ(advertisement.scan_response.local_name()->name, "robot");
  EXPECT_FALSE(advertisement.anonymous);
}

TEST_F(PeripheralTest,
       StartAdvertisingWithExtendedProcedureWithConnectionOptionsNonBondable) {
  Peripheral2::AdvertisingParameters parameters;
  Peripheral2::ConnectionOptions connection_options{
      .bondable_mode = false,
      .service_filter = std::nullopt,
      .parameters = std::nullopt,
      .att_mtu = std::nullopt};
  parameters.procedure = Peripheral2::ExtendedAdvertising{
      .configuration = connection_options,
      .tx_power = std::nullopt,
      .primary_phy = pw::bluetooth::low_energy::Phy::k1Megabit,
      .secondary_phy = pw::bluetooth::low_energy::Phy::k1Megabit};

  AdvertisedPeripheral2::Ptr advertised_peripheral =
      AdvertiseExpectSuccess(parameters);

  ASSERT_EQ(adapter().fake_le()->registered_advertisements().size(), 1u);
  auto& advertisement =
      adapter().fake_le()->registered_advertisements().begin()->second;
  ASSERT_TRUE(advertisement.connectable.has_value());
  EXPECT_EQ(advertisement.connectable->bondable_mode,
            bt::sm::BondableMode::NonBondable);
}

TEST_F(PeripheralTest,
       StartAdvertisingWithExtendedProcedureWithConnectionOptionsBondable) {
  Peripheral2::AdvertisingParameters parameters;
  Peripheral2::ConnectionOptions connection_options{
      .bondable_mode = true,
      .service_filter = std::nullopt,
      .parameters = std::nullopt,
      .att_mtu = std::nullopt};
  parameters.procedure = Peripheral2::ExtendedAdvertising{
      .configuration = connection_options,
      .tx_power = std::nullopt,
      .primary_phy = pw::bluetooth::low_energy::Phy::k1Megabit,
      .secondary_phy = pw::bluetooth::low_energy::Phy::k1Megabit};

  AdvertisedPeripheral2::Ptr advertised_peripheral =
      AdvertiseExpectSuccess(parameters);

  ASSERT_EQ(adapter().fake_le()->registered_advertisements().size(), 1u);
  auto& advertisement =
      adapter().fake_le()->registered_advertisements().begin()->second;
  ASSERT_TRUE(advertisement.connectable.has_value());
  EXPECT_EQ(advertisement.connectable->bondable_mode,
            bt::sm::BondableMode::Bondable);
}

TEST_F(PeripheralTest, StartAdvertisingFailureInternalError) {
  adapter().fake_le()->set_advertising_result(
      ToResult(bt::HostError::kScanResponseTooLong));
  Peripheral2::AdvertisingParameters parameters;
  std::optional<Peripheral2::AdvertiseResult> result = Advertise(parameters);
  ASSERT_TRUE(result.has_value());
  ASSERT_FALSE(result.value().has_value());
  EXPECT_EQ(result.value().error(), AdvertiseError::kScanResponseDataTooLong);
}

TEST_F(PeripheralTest,
       StartAdvertisingAndCallAdvertisedPeripheralStopAdvertising) {
  Peripheral2::AdvertisingParameters parameters;

  AdvertisedPeripheral2::Ptr advertised_peripheral =
      AdvertiseExpectSuccess(parameters);
  ASSERT_EQ(adapter().fake_le()->registered_advertisements().size(), 1u);

  advertised_peripheral->StopAdvertising();

  pw::async2::FuncTask stop_task(
      [&advertised_peripheral](pw::async2::Context& cx) -> pw::async2::Poll<> {
        pw::async2::Poll<pw::Status> pend = advertised_peripheral->PendStop(cx);
        if (pend.IsReady()) {
          EXPECT_TRUE(pend->ok());
          return pw::async2::Ready();
        }
        return pw::async2::Pending();
      });
  dispatcher2().Post(stop_task);

  EXPECT_TRUE(dispatcher2().RunUntilStalled());
  ASSERT_EQ(adapter().fake_le()->registered_advertisements().size(), 1u);

  // Process the stop request.
  dispatcher().RunUntilIdle();
  // Process the waker wake.
  dispatcher2().RunToCompletion();
  ASSERT_EQ(adapter().fake_le()->registered_advertisements().size(), 0u);
}
