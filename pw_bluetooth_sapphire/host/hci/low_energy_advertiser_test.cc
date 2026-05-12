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

#include "pw_bluetooth_sapphire/internal/host/hci/low_energy_advertiser.h"

#include "pw_bluetooth_sapphire/internal/host/hci/android_extended_low_energy_advertiser.h"
#include "pw_bluetooth_sapphire/internal/host/hci/extended_low_energy_advertiser.h"
#include "pw_bluetooth_sapphire/internal/host/hci/legacy_low_energy_advertiser.h"
#include "pw_bluetooth_sapphire/internal/host/testing/controller_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_controller.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_peer.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"
#include "pw_bluetooth_sapphire/internal/host/transport/control_packets.h"

// LowEnergyAdvertiser has many potential subclasses (e.g.
// LegacyLowEnergyAdvertiser, ExtendedLowEnergyAdvertiser,
// AndroidExtendedLowEnergyAdvertiser, etc). The unique features of these
// subclass are tested individually in their own unittest files. However, there
// are some common features that all LowEnergyAdvertisers should follow. This
// class implements a type parameterized test to exercise those common features.
//
// If you add a new subclass of LowEnergyAdvertiser in the future, make sure to
// add its type to the list of types below (in the TYPED_TEST_SUITE) so that its
// common features are exercised as well.

namespace bt::hci {
namespace {

namespace pwemb = pw::bluetooth::emboss;

using bt::testing::FakeController;
using bt::testing::FakePeer;

using AdvertisingOptions = LowEnergyAdvertiser::AdvertisingOptions;
using TestingBase = bt::testing::FakeDispatcherControllerTest<FakeController>;

constexpr hci_spec::ConnectionHandle kConnectionHandle = 0x0001;
constexpr AdvertisingIntervalRange kTestInterval(
    hci_spec::kLEAdvertisingIntervalMin, hci_spec::kLEAdvertisingIntervalMax);

const DeviceAddress kPublicAddress(DeviceAddress::Type::kLEPublic, {1});
const DeviceAddress kRandomAddress(DeviceAddress::Type::kLERandom, {2});

TEST(AdvertisingIntervalRangeDeathTest, MinLessThanMax) {
  EXPECT_DEATH(AdvertisingIntervalRange(hci_spec::kLEAdvertisingIntervalMax,
                                        hci_spec::kLEAdvertisingIntervalMin),
               ".*");
}

template <typename T>
class LowEnergyAdvertiserTest : public TestingBase {
 public:
  LowEnergyAdvertiserTest() = default;
  ~LowEnergyAdvertiserTest() override = default;

 protected:
  void SetUp() override {
    TestingBase::SetUp();

    // ACL data channel needs to be present for production hci::Connection
    // objects.
    TestingBase::InitializeACLDataChannel(
        hci::DataBufferInfo(),
        hci::DataBufferInfo(hci_spec::kMaxACLPayloadSize, 10));

    FakeController::Settings settings;
    settings.ApplyLegacyLEConfig();
    settings.bd_addr = kPublicAddress;
    test_device()->set_settings(settings);

    advertiser_ = std::unique_ptr<T>(CreateAdvertiserInternal());

    test_device()->set_num_supported_advertising_sets(0xEF);
  }

  void TearDown() override {
    advertiser_ = nullptr;
    TestingBase::TearDown();
  }

  template <bool same = std::is_same_v<T, AndroidExtendedLowEnergyAdvertiser>>
  std::enable_if_t<same, AndroidExtendedLowEnergyAdvertiser>*
  CreateAdvertiserInternal() {
    return new AndroidExtendedLowEnergyAdvertiser(
        transport()->GetWeakPtr(), dispatcher(), 1);
  }

  template <bool same = std::is_same_v<T, ExtendedLowEnergyAdvertiser>>
  std::enable_if_t<same, ExtendedLowEnergyAdvertiser>*
  CreateAdvertiserInternal() {
    return new ExtendedLowEnergyAdvertiser(
        transport()->GetWeakPtr(),
        dispatcher(),
        hci_spec::kMaxLEExtendedAdvertisingDataLength);
  }

  template <bool same = std::is_same_v<T, LegacyLowEnergyAdvertiser>>
  std::enable_if_t<same, LegacyLowEnergyAdvertiser>*
  CreateAdvertiserInternal() {
    return new LegacyLowEnergyAdvertiser(transport()->GetWeakPtr(),
                                         dispatcher());
  }

  LowEnergyAdvertiser* advertiser() const { return advertiser_.get(); }

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
      EXPECT_TRUE(status.is_error());
    };
  }

  std::optional<Result<AdvertisementId>> TakeLastStatus() {
    if (!last_status_) {
      return std::nullopt;
    }

    Result<AdvertisementId> status = last_status_.value();
    last_status_.reset();
    return status;
  }

  // Makes some fake advertising data.
  // |include_flags| signals whether to include flag encoding size in the data
  // calculation.
  AdvertisingData GetExampleData(bool include_flags = true) {
    AdvertisingData result;

    auto name = "fuchsia";
    EXPECT_TRUE(result.SetLocalName(name));

    uint16_t appearance = 0x1234;
    result.SetAppearance(appearance);

    EXPECT_LE(result.CalculateBlockSize(include_flags),
              hci_spec::kMaxLEAdvertisingDataLength);
    return result;
  }

  // Makes fake advertising data that is too large.
  // |include_flags| signals whether to include flag encoding size in the data
  // calculation.
  AdvertisingData GetTooLargeExampleData(
      bool include_flags,
      std::size_t size = hci_spec::kMaxLEAdvertisingDataLength + 1) {
    AdvertisingData result;

    if (include_flags) {
      size -= kTLVFlagsSize;
    }

    std::string data(size + 1, 'a');
    EXPECT_TRUE(result.SetLocalName(data));

    // The maximum advertisement packet is:
    // |hci_spec::kMaxLEAdvertisingDataLength| = 31, and |result| = 32 bytes.
    // |result| should be too large to advertise.
    EXPECT_GT(result.CalculateBlockSize(include_flags),
              hci_spec::kMaxLEAdvertisingDataLength);
    return result;
  }

  std::optional<hci_spec::AdvertisingHandle> CurrentAdvertisingHandle() const {
    if (std::is_same_v<T, ExtendedLowEnergyAdvertiser>) {
      auto extended = static_cast<ExtendedLowEnergyAdvertiser*>(advertiser());
      return extended->LastUsedHandleForTesting();
    }

    if (std::is_same_v<T, AndroidExtendedLowEnergyAdvertiser>) {
      auto extended =
          static_cast<AndroidExtendedLowEnergyAdvertiser*>(advertiser());
      return extended->LastUsedHandleForTesting();
    }

    return 0;  // non-extended advertising doesn't use handles, we can return
               // any value
  }

  const FakeController::LEAdvertisingState& GetControllerAdvertisingState()
      const {
    if (std::is_same_v<T, LegacyLowEnergyAdvertiser>) {
      return test_device()->legacy_advertising_state();
    }

    if (std::is_same_v<T, ExtendedLowEnergyAdvertiser> ||
        std::is_same_v<T, AndroidExtendedLowEnergyAdvertiser>) {
      std::optional<hci_spec::AdvertisingHandle> handle =
          CurrentAdvertisingHandle();
      if (!handle) {
        static FakeController::LEAdvertisingState empty;
        return empty;
      }

      return test_device()->extended_advertising_state(handle.value());
    }

    EXPECT_TRUE(false) << "advertiser is of unknown type";

    // return something in order to compile, tests will fail if they get here
    static FakeController::LEAdvertisingState state;
    return state;
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

  void SetRandomAddress(DeviceAddress random_address) {
    if (std::is_same_v<T, LegacyLowEnergyAdvertiser>) {
      auto emboss_packet = CommandPacket::New<
          pw::bluetooth::emboss::LESetRandomAddressCommandWriter>(
          hci_spec::kLESetRandomAddress);
      emboss_packet.view_t().random_address().CopyFrom(
          random_address.value().view());

      test_device()->SendCommand(emboss_packet.data().subspan());
      RunUntilIdle();
    }

    // TODO(fxbug.dev/42161900): Support setting a random address
    // for extended advertisers when proper checks are introduced in
    // `FakeController::OnLESetExtendedAdvertisingEnable()`.
  }

  bool SupportsExtendedPdus() const {
    return std::is_same_v<T, ExtendedLowEnergyAdvertiser>;
  }

 private:
  std::unique_ptr<LowEnergyAdvertiser> advertiser_;
  std::optional<Result<AdvertisementId>> last_status_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(LowEnergyAdvertiserTest);
};

using Implementations = ::testing::Types<LegacyLowEnergyAdvertiser,
                                         ExtendedLowEnergyAdvertiser,
                                         AndroidExtendedLowEnergyAdvertiser>;
TYPED_TEST_SUITE(LowEnergyAdvertiserTest, Implementations);

TYPED_TEST(LowEnergyAdvertiserTest, GetLegacyAdvertisingEventPropertiesAdvInd) {
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*extended_pdu=*/false,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/false);
  LowEnergyAdvertiser::AdvertisingEventProperties properties =
      LowEnergyAdvertiser::GetAdvertisingEventProperties(this->GetExampleData(),
                                                         this->GetExampleData(),
                                                         options,
                                                         [](auto, auto) {});
  EXPECT_TRUE(properties.connectable);
  EXPECT_TRUE(properties.scannable);
  EXPECT_FALSE(properties.directed);
  EXPECT_FALSE(properties.high_duty_cycle_directed_connectable);
  EXPECT_TRUE(properties.use_legacy_pdus);
  EXPECT_FALSE(properties.anonymous_advertising);
  EXPECT_FALSE(properties.include_tx_power);
}

TYPED_TEST(LowEnergyAdvertiserTest,
           GetLegacyAdvertisingEventPropertiesAdvScanInd) {
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*extended_pdu=*/false,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/false);
  LowEnergyAdvertiser::AdvertisingEventProperties properties =
      LowEnergyAdvertiser::GetAdvertisingEventProperties(
          this->GetExampleData(),
          this->GetExampleData(),
          options,
          /*connect_callback=*/nullptr);
  EXPECT_FALSE(properties.connectable);
  EXPECT_TRUE(properties.scannable);
  EXPECT_FALSE(properties.directed);
  EXPECT_FALSE(properties.high_duty_cycle_directed_connectable);
  EXPECT_TRUE(properties.use_legacy_pdus);
  EXPECT_FALSE(properties.anonymous_advertising);
  EXPECT_FALSE(properties.include_tx_power);
}

TYPED_TEST(LowEnergyAdvertiserTest,
           GetLegacyAdvertisingEventPropertiesAdvNonConnInd) {
  AdvertisingData empty;
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*extended_pdu=*/false,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/false);
  LowEnergyAdvertiser::AdvertisingEventProperties properties =
      LowEnergyAdvertiser::GetAdvertisingEventProperties(
          this->GetExampleData(),
          empty,
          options,
          /*connect_callback=*/nullptr);
  EXPECT_FALSE(properties.connectable);
  EXPECT_FALSE(properties.scannable);
  EXPECT_FALSE(properties.directed);
  EXPECT_FALSE(properties.high_duty_cycle_directed_connectable);
  EXPECT_TRUE(properties.use_legacy_pdus);
  EXPECT_FALSE(properties.anonymous_advertising);
  EXPECT_FALSE(properties.include_tx_power);
}

// Ensure we can't set include anonymous for legacy pdus
TYPED_TEST(LowEnergyAdvertiserTest,
           GetLegacyAdvertisingEventPropertiesNoAnonymous) {
  AdvertisingData empty;
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*extended_pdu=*/false,
                             /*anonymous=*/true,
                             /*include_tx_power_level=*/false);
  LowEnergyAdvertiser::AdvertisingEventProperties properties =
      LowEnergyAdvertiser::GetAdvertisingEventProperties(
          empty,
          empty,
          options,
          /*connect_callback=*/nullptr);
  EXPECT_FALSE(properties.connectable);
  EXPECT_FALSE(properties.scannable);
  EXPECT_FALSE(properties.directed);
  EXPECT_FALSE(properties.high_duty_cycle_directed_connectable);
  EXPECT_TRUE(properties.use_legacy_pdus);
  EXPECT_FALSE(properties.anonymous_advertising);
  EXPECT_FALSE(properties.include_tx_power);
}

// Ensure we can't set include tx power level for legacy pdus
TYPED_TEST(LowEnergyAdvertiserTest,
           GetLegacyAdvertisingEventPropertiesNoIncludeTxPower) {
  AdvertisingData empty;
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*extended_pdu=*/false,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/true);
  LowEnergyAdvertiser::AdvertisingEventProperties properties =
      LowEnergyAdvertiser::GetAdvertisingEventProperties(
          empty,
          empty,
          options,
          /*connect_callback=*/nullptr);
  EXPECT_FALSE(properties.connectable);
  EXPECT_FALSE(properties.scannable);
  EXPECT_FALSE(properties.directed);
  EXPECT_FALSE(properties.high_duty_cycle_directed_connectable);
  EXPECT_TRUE(properties.use_legacy_pdus);
  EXPECT_FALSE(properties.anonymous_advertising);
  EXPECT_FALSE(properties.include_tx_power);
}

TYPED_TEST(LowEnergyAdvertiserTest,
           GetExtendedAdvertisingEventPropertiesConnectable) {
  AdvertisingData empty;
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*extended_pdu=*/true,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/false);
  LowEnergyAdvertiser::AdvertisingEventProperties properties =
      LowEnergyAdvertiser::GetAdvertisingEventProperties(
          empty, empty, options, [](auto, auto) {});
  EXPECT_TRUE(properties.connectable);
  EXPECT_FALSE(properties.scannable);
  EXPECT_FALSE(properties.directed);
  EXPECT_FALSE(properties.high_duty_cycle_directed_connectable);
  EXPECT_FALSE(properties.use_legacy_pdus);
  EXPECT_FALSE(properties.anonymous_advertising);
  EXPECT_FALSE(properties.include_tx_power);
}

TYPED_TEST(LowEnergyAdvertiserTest,
           GetExtendedAdvertisingEventPropertiesScannable) {
  AdvertisingData empty;
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*extended_pdu=*/true,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/false);
  LowEnergyAdvertiser::AdvertisingEventProperties properties =
      LowEnergyAdvertiser::GetAdvertisingEventProperties(
          empty,
          this->GetExampleData(),
          options,
          /*connect_callback=*/nullptr);
  EXPECT_FALSE(properties.connectable);
  EXPECT_TRUE(properties.scannable);
  EXPECT_FALSE(properties.directed);
  EXPECT_FALSE(properties.high_duty_cycle_directed_connectable);
  EXPECT_FALSE(properties.use_legacy_pdus);
  EXPECT_FALSE(properties.anonymous_advertising);
  EXPECT_FALSE(properties.include_tx_power);
}

TYPED_TEST(LowEnergyAdvertiserTest,
           GetExtendedAdvertisingEventPropertiesAnonymous) {
  AdvertisingData empty;
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*extended_pdu=*/true,
                             /*anonymous=*/true,
                             /*include_tx_power_level=*/false);
  LowEnergyAdvertiser::AdvertisingEventProperties properties =
      LowEnergyAdvertiser::GetAdvertisingEventProperties(
          empty,
          empty,
          options,
          /*connect_callback=*/nullptr);
  EXPECT_FALSE(properties.connectable);
  EXPECT_FALSE(properties.scannable);
  EXPECT_FALSE(properties.directed);
  EXPECT_FALSE(properties.high_duty_cycle_directed_connectable);
  EXPECT_FALSE(properties.use_legacy_pdus);
  EXPECT_TRUE(properties.anonymous_advertising);
  EXPECT_FALSE(properties.include_tx_power);
}

TYPED_TEST(LowEnergyAdvertiserTest,
           GetExtendedAdvertisingEventPropertiesIncludeTxPower) {
  AdvertisingData empty;
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*extended_pdu=*/true,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/true);
  LowEnergyAdvertiser::AdvertisingEventProperties properties =
      LowEnergyAdvertiser::GetAdvertisingEventProperties(
          empty,
          empty,
          options,
          /*connect_callback=*/nullptr);
  EXPECT_FALSE(properties.connectable);
  EXPECT_FALSE(properties.scannable);
  EXPECT_FALSE(properties.directed);
  EXPECT_FALSE(properties.high_duty_cycle_directed_connectable);
  EXPECT_FALSE(properties.use_legacy_pdus);
  EXPECT_FALSE(properties.anonymous_advertising);
  EXPECT_TRUE(properties.include_tx_power);
}

TYPED_TEST(LowEnergyAdvertiserTest,
           AdvertisingEventPropertiesToLEAdvertisingType) {
  {
    LowEnergyAdvertiser::AdvertisingEventProperties properties;
    properties.scannable = true;
    properties.connectable = true;
    pwemb::LEAdvertisingType adv_type =
        LowEnergyAdvertiser::AdvertisingEventPropertiesToLEAdvertisingType(
            properties);
    EXPECT_EQ(adv_type,
              pwemb::LEAdvertisingType::CONNECTABLE_AND_SCANNABLE_UNDIRECTED);
  }

  {
    LowEnergyAdvertiser::AdvertisingEventProperties properties;
    properties.directed = true;
    properties.connectable = true;
    pwemb::LEAdvertisingType adv_type =
        LowEnergyAdvertiser::AdvertisingEventPropertiesToLEAdvertisingType(
            properties);
    EXPECT_EQ(adv_type,
              pwemb::LEAdvertisingType::CONNECTABLE_LOW_DUTY_CYCLE_DIRECTED);
  }

  {
    LowEnergyAdvertiser::AdvertisingEventProperties properties;
    properties.high_duty_cycle_directed_connectable = true;
    properties.directed = true;
    properties.connectable = true;
    pwemb::LEAdvertisingType adv_type =
        LowEnergyAdvertiser::AdvertisingEventPropertiesToLEAdvertisingType(
            properties);
    EXPECT_EQ(adv_type,
              pwemb::LEAdvertisingType::CONNECTABLE_HIGH_DUTY_CYCLE_DIRECTED);
  }

  {
    LowEnergyAdvertiser::AdvertisingEventProperties properties;
    properties.scannable = true;
    pwemb::LEAdvertisingType adv_type =
        LowEnergyAdvertiser::AdvertisingEventPropertiesToLEAdvertisingType(
            properties);
    EXPECT_EQ(adv_type, pwemb::LEAdvertisingType::SCANNABLE_UNDIRECTED);
  }

  {
    LowEnergyAdvertiser::AdvertisingEventProperties properties;
    pwemb::LEAdvertisingType adv_type =
        LowEnergyAdvertiser::AdvertisingEventPropertiesToLEAdvertisingType(
            properties);
    EXPECT_EQ(adv_type, pwemb::LEAdvertisingType::NOT_CONNECTABLE_UNDIRECTED);
  }
}

// - Stops the advertisement when an incoming connection comes in
// - Possible to restart advertising
// - Advertising state cleaned up between calls
TYPED_TEST(LowEnergyAdvertiserTest, ConnectionTest) {
  AdvertisingData adv_data = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*extended_pdu=*/false,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/false);

  std::unique_ptr<LowEnergyConnection> link;
  auto conn_cb = [&link](auto, auto cb_link) { link = std::move(cb_link); };

  // Start advertising kPublicAddress
  this->advertiser()->StartAdvertising(kPublicAddress,
                                       adv_data,
                                       scan_data,
                                       options,
                                       conn_cb,
                                       this->MakeExpectSuccessCallback());
  this->RunUntilIdle();
  std::optional<Result<AdvertisementId>> status_public = this->TakeLastStatus();
  ASSERT_TRUE(status_public.has_value());
  ASSERT_TRUE(status_public->is_ok());
  AdvertisementId adv_id_public = status_public->value();
  EXPECT_TRUE(this->advertiser()->IsAdvertising());
  EXPECT_TRUE(this->advertiser()->IsAdvertising(adv_id_public));

  std::optional<hci_spec::AdvertisingHandle> adv_handle_public =
      this->CurrentAdvertisingHandle();
  ASSERT_TRUE(adv_handle_public);

  // Accept a connection and ensure that connection state is set up correctly
  link.reset();
  this->advertiser()->OnIncomingConnection(kConnectionHandle,
                                           pwemb::ConnectionRole::PERIPHERAL,
                                           kRandomAddress,
                                           hci_spec::LEConnectionParameters());
  this->SendMultipleAdvertisingPostConnectionEvents(kConnectionHandle,
                                                    adv_handle_public.value());
  this->RunUntilIdle();

  ASSERT_TRUE(link);
  EXPECT_EQ(kConnectionHandle, link->handle());
  EXPECT_EQ(kPublicAddress, link->local_address());
  EXPECT_EQ(kRandomAddress, link->peer_address());
  EXPECT_FALSE(this->advertiser()->IsAdvertising());
  EXPECT_FALSE(this->advertiser()->IsAdvertising(adv_id_public));

  // Advertising state should get cleared on a disconnection
  link->Disconnect(pwemb::StatusCode::REMOTE_USER_TERMINATED_CONNECTION);
  this->test_device()->SendDisconnectionCompleteEvent(link->handle());
  this->RunUntilIdle();
  EXPECT_FALSE(this->GetControllerAdvertisingState().enabled);

  // Restart advertising using a different local address
  this->SetRandomAddress(kRandomAddress);
  this->advertiser()->StartAdvertising(kRandomAddress,
                                       adv_data,
                                       scan_data,
                                       options,
                                       conn_cb,
                                       this->MakeExpectSuccessCallback());
  this->RunUntilIdle();
  std::optional<Result<AdvertisementId>> status_random = this->TakeLastStatus();
  ASSERT_TRUE(status_random.has_value());
  ASSERT_TRUE(status_random->is_ok());
  EXPECT_TRUE(this->GetControllerAdvertisingState().enabled);

  std::optional<hci_spec::AdvertisingHandle> adv_handle_random =
      this->CurrentAdvertisingHandle();
  ASSERT_TRUE(adv_handle_random);

  // Accept a connection from kPublicAddress. The internal advertising state
  // should get assigned correctly with no remnants of the previous advertise.
  link.reset();
  this->advertiser()->OnIncomingConnection(kConnectionHandle,
                                           pwemb::ConnectionRole::PERIPHERAL,
                                           kPublicAddress,
                                           hci_spec::LEConnectionParameters());
  this->SendMultipleAdvertisingPostConnectionEvents(kConnectionHandle,
                                                    adv_handle_random.value());
  this->RunUntilIdle();

  ASSERT_TRUE(link);
  EXPECT_EQ(kRandomAddress, link->local_address());
  EXPECT_EQ(kPublicAddress, link->peer_address());
}

// Tests that advertising can be restarted right away in a connection
// callback.
TYPED_TEST(LowEnergyAdvertiserTest, RestartInConnectionCallback) {
  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*extended_pdu=*/false,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/false);

  std::unique_ptr<LowEnergyConnection> link;
  auto conn_cb = [&, this](auto, auto cb_link) {
    link = std::move(cb_link);

    this->advertiser()->StartAdvertising(
        kPublicAddress,
        ad,
        scan_data,
        options,
        [](auto, auto) { /*noop*/ },
        this->MakeExpectSuccessCallback());
  };

  this->advertiser()->StartAdvertising(kPublicAddress,
                                       ad,
                                       scan_data,
                                       options,
                                       conn_cb,
                                       this->MakeExpectSuccessCallback());
  this->RunUntilIdle();
  EXPECT_TRUE(this->TakeLastStatus());
  EXPECT_TRUE(this->GetControllerAdvertisingState().enabled);

  bool enabled = true;
  std::vector<bool> adv_states;
  this->test_device()->set_advertising_state_callback(
      [this, &adv_states, &enabled] {
        bool new_enabled = this->GetControllerAdvertisingState().enabled;
        if (enabled != new_enabled) {
          adv_states.push_back(new_enabled);
          enabled = new_enabled;
        }
      });

  this->advertiser()->OnIncomingConnection(kConnectionHandle,
                                           pwemb::ConnectionRole::PERIPHERAL,
                                           kRandomAddress,
                                           hci_spec::LEConnectionParameters());
  std::optional<hci_spec::AdvertisingHandle> handle =
      this->CurrentAdvertisingHandle();
  ASSERT_TRUE(handle);
  this->SendMultipleAdvertisingPostConnectionEvents(kConnectionHandle,
                                                    handle.value());

  // Advertising should get disabled and re-enabled.
  this->RunUntilIdle();
  ASSERT_EQ(2u, adv_states.size());
  EXPECT_FALSE(adv_states[0]);
  EXPECT_TRUE(adv_states[1]);

  this->test_device()->set_advertising_state_callback(nullptr);
}

// An incoming connection when not advertising should get disconnected.
TYPED_TEST(LowEnergyAdvertiserTest, IncomingConnectionWhenNotAdvertising) {
  std::vector<std::pair<bool, hci_spec::ConnectionHandle>> connection_states;
  this->test_device()->set_connection_state_callback(
      [&](const auto& address, auto handle, bool connected, bool canceled) {
        EXPECT_EQ(kRandomAddress, address);
        EXPECT_FALSE(canceled);
        connection_states.push_back(std::make_pair(connected, handle));
      });

  auto fake_peer = std::make_unique<FakePeer>(
      kRandomAddress, TestFixture::dispatcher(), true, true);
  this->test_device()->AddPeer(std::move(fake_peer));
  this->test_device()->ConnectLowEnergy(kRandomAddress,
                                        pwemb::ConnectionRole::PERIPHERAL);
  this->RunUntilIdle();

  ASSERT_EQ(1u, connection_states.size());
  auto [connection_state, handle] = connection_states[0];
  EXPECT_TRUE(connection_state);

  // Notify the advertiser of the incoming connection. It should reject it and
  // the controller should become disconnected.
  this->advertiser()->OnIncomingConnection(handle,
                                           pwemb::ConnectionRole::PERIPHERAL,
                                           kRandomAddress,
                                           hci_spec::LEConnectionParameters());
  this->SendMultipleAdvertisingPostConnectionEvents(kConnectionHandle, 0);
  this->RunUntilIdle();
  ASSERT_EQ(2u, connection_states.size());
  auto [connection_state_after_disconnect, disconnected_handle] =
      connection_states[1];
  EXPECT_EQ(handle, disconnected_handle);
  EXPECT_FALSE(connection_state_after_disconnect);
}

// An incoming connection during non-connectable advertising should get
// disconnected.
TYPED_TEST(LowEnergyAdvertiserTest,
           IncomingConnectionWhenNonConnectableAdvertising) {
  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*extended_pdu=*/false,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/false);
  this->advertiser()->StartAdvertising(kPublicAddress,
                                       ad,
                                       scan_data,
                                       options,
                                       nullptr,
                                       this->MakeExpectSuccessCallback());
  this->RunUntilIdle();
  ASSERT_TRUE(this->TakeLastStatus());

  std::vector<std::pair<bool, hci_spec::ConnectionHandle>> connection_states;
  this->test_device()->set_connection_state_callback(
      [&](const auto& address, auto handle, bool connected, bool canceled) {
        EXPECT_EQ(kRandomAddress, address);
        EXPECT_FALSE(canceled);
        connection_states.push_back(std::make_pair(connected, handle));
      });

  auto fake_peer = std::make_unique<FakePeer>(
      kRandomAddress, TestFixture::dispatcher(), true, true);
  this->test_device()->AddPeer(std::move(fake_peer));
  this->test_device()->ConnectLowEnergy(kRandomAddress,
                                        pwemb::ConnectionRole::PERIPHERAL);
  this->RunUntilIdle();

  ASSERT_EQ(1u, connection_states.size());
  auto [connection_state, handle] = connection_states[0];
  EXPECT_TRUE(connection_state);

  // Notify the advertiser of the incoming connection. It should reject it and
  // the controller should become disconnected.
  this->advertiser()->OnIncomingConnection(handle,
                                           pwemb::ConnectionRole::PERIPHERAL,
                                           kRandomAddress,
                                           hci_spec::LEConnectionParameters());
  this->SendMultipleAdvertisingPostConnectionEvents(kConnectionHandle, 0);
  this->RunUntilIdle();
  ASSERT_EQ(2u, connection_states.size());
  auto [connection_state_after_disconnect, disconnected_handle] =
      connection_states[1];
  EXPECT_EQ(handle, disconnected_handle);
  EXPECT_FALSE(connection_state_after_disconnect);
}

// Tests starting and stopping an advertisement.
TYPED_TEST(LowEnergyAdvertiserTest, StartAndStop) {
  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*extended_pdu=*/false,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/false);
  this->SetRandomAddress(kRandomAddress);

  this->advertiser()->StartAdvertising(kRandomAddress,
                                       ad,
                                       scan_data,
                                       options,
                                       nullptr,
                                       this->MakeExpectSuccessCallback());
  this->RunUntilIdle();
  std::optional<Result<AdvertisementId>> status = this->TakeLastStatus();
  ASSERT_TRUE(status);
  ASSERT_TRUE(status->is_ok());
  AdvertisementId adv_id = status->value();
  EXPECT_TRUE(this->GetControllerAdvertisingState().enabled);

  std::optional<hci::Result<>> stop_result;
  this->advertiser()->StopAdvertising(
      adv_id, [&stop_result](hci::Result<> result) { stop_result = result; });

  this->RunUntilIdle();
  EXPECT_FALSE(this->GetControllerAdvertisingState().enabled);
  ASSERT_TRUE(stop_result.has_value());
  EXPECT_TRUE(stop_result->is_ok());
}

// Tests that an advertisement is configured with the correct parameters.
TYPED_TEST(LowEnergyAdvertiserTest, AdvertisingParameters) {
  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();
  auto flags = AdvFlag::kLEGeneralDiscoverableMode;
  AdvertisingOptions options(kTestInterval,
                             flags,
                             /*extended_pdu=*/false,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/false);
  this->SetRandomAddress(kRandomAddress);

  this->advertiser()->StartAdvertising(kRandomAddress,
                                       ad,
                                       scan_data,
                                       options,
                                       nullptr,
                                       this->MakeExpectSuccessCallback());
  this->RunUntilIdle();
  std::optional<Result<AdvertisementId>> status = this->TakeLastStatus();
  ASSERT_TRUE(status);
  ASSERT_TRUE(status->is_ok());
  AdvertisementId adv_id = status->value();

  // The expected advertisement including the Flags.
  DynamicByteBuffer expected_ad(ad.CalculateBlockSize(/*include_flags=*/true));
  ad.WriteBlock(&expected_ad, flags);

  DynamicByteBuffer expected_scan_data(
      scan_data.CalculateBlockSize(/*include_flags=*/false));
  scan_data.WriteBlock(&expected_scan_data, std::nullopt);

  std::optional<FakeController::LEAdvertisingState> state =
      this->GetControllerAdvertisingState();
  EXPECT_TRUE(state);
  EXPECT_TRUE(state->enabled);
  EXPECT_EQ(kTestInterval.min(), state->interval_min);
  EXPECT_EQ(kTestInterval.max(), state->interval_max);
  EXPECT_EQ(expected_ad, state->advertised_view());
  EXPECT_EQ(expected_scan_data, state->scan_rsp_view());
  EXPECT_EQ(pwemb::LEOwnAddressType::RANDOM, state->own_address_type);

  // Restart advertising with a public address and verify that the configured
  // local address type is correct.
  std::optional<hci::Result<>> stop_result;
  this->advertiser()->StopAdvertising(
      adv_id, [&stop_result](hci::Result<> result) { stop_result = result; });
  AdvertisingOptions new_options(kTestInterval,
                                 kDefaultNoAdvFlags,
                                 /*extended_pdu=*/false,
                                 /*anonymous=*/false,
                                 /*include_tx_power_level=*/false);
  this->advertiser()->StartAdvertising(kPublicAddress,
                                       ad,
                                       scan_data,
                                       new_options,
                                       nullptr,
                                       this->MakeExpectSuccessCallback());
  this->RunUntilIdle();
  ASSERT_TRUE(stop_result.has_value());
  EXPECT_TRUE(stop_result->is_ok());
  EXPECT_TRUE(this->TakeLastStatus());

  state = this->GetControllerAdvertisingState();
  EXPECT_TRUE(state);
  EXPECT_TRUE(state->enabled);
  EXPECT_EQ(pwemb::LEOwnAddressType::PUBLIC, state->own_address_type);
}

// Tests that a previous advertisement's advertising data isn't accidentally
// kept around. This test was added to address fxbug.dev/42081491.
TYPED_TEST(LowEnergyAdvertiserTest, PreviousAdvertisingParameters) {
  AdvertisingData scan_data = this->GetExampleData();
  auto flags = AdvFlag::kLEGeneralDiscoverableMode;
  AdvertisingOptions options(kTestInterval,
                             flags,
                             /*extended_pdu=*/false,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/false);

  // old advertising data: ideally we would fill this completely so that in
  // the next start advertising call, we can confirm that none of the remnants
  // are left. However, doing so results in the advertising data being too
  // long. Instead, we rely on other unit tests within advertising data
  // unittests to ensure all previous remnants are removed.
  AdvertisingData ad;
  ad.SetTxPower(4);
  ad.SetAppearance(0x4567);
  EXPECT_TRUE(ad.SetLocalName("fuchsia"));
  this->SetRandomAddress(kRandomAddress);

  this->advertiser()->StartAdvertising(kRandomAddress,
                                       ad,
                                       scan_data,
                                       options,
                                       nullptr,
                                       this->MakeExpectSuccessCallback());
  this->RunUntilIdle();
  std::optional<Result<AdvertisementId>> status = this->TakeLastStatus();
  ASSERT_TRUE(status);
  ASSERT_TRUE(status->is_ok());
  AdvertisementId adv_id = status->value();

  // new advertising data (with fewer fields filled in)
  std::optional<hci::Result<>> stop_result;
  this->advertiser()->StopAdvertising(
      adv_id, [&stop_result](hci::Result<> result) { stop_result = result; });
  AdvertisingData new_ad = this->GetExampleData();
  this->advertiser()->StartAdvertising(kRandomAddress,
                                       new_ad,
                                       scan_data,
                                       options,
                                       nullptr,
                                       this->MakeExpectSuccessCallback());
  this->RunUntilIdle();
  EXPECT_TRUE(this->TakeLastStatus());
  ASSERT_TRUE(stop_result.has_value());
  EXPECT_TRUE(stop_result->is_ok());

  DynamicByteBuffer expected_ad(
      new_ad.CalculateBlockSize(/*include_flags=*/true));
  new_ad.WriteBlock(&expected_ad, flags);

  std::optional<FakeController::LEAdvertisingState> state =
      this->GetControllerAdvertisingState();
  EXPECT_EQ(expected_ad, state->advertised_view());
}

// Tests that advertising interval values are passed down correctly.
TYPED_TEST(LowEnergyAdvertiserTest, AdvertisingIntervalValues) {
  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();

  const AdvertisingIntervalRange interval(
      hci_spec::kLEAdvertisingIntervalMin - 1,
      hci_spec::kLEAdvertisingIntervalMax + 1);
  AdvertisingOptions options(interval,
                             kDefaultNoAdvFlags,
                             /*init_extended_pdu=*/false,
                             /*init_anonymous=*/false,
                             /*init_include_tx_power_level=*/false);
  this->SetRandomAddress(kRandomAddress);

  this->advertiser()->StartAdvertising(kRandomAddress,
                                       ad,
                                       scan_data,
                                       options,
                                       nullptr,
                                       this->MakeExpectErrorCallback());
  this->RunUntilIdle();
  EXPECT_TRUE(this->TakeLastStatus());

  std::optional<FakeController::LEAdvertisingState> state =
      this->GetControllerAdvertisingState();
  EXPECT_TRUE(state);
  EXPECT_EQ(0u, state->interval_min);
  EXPECT_EQ(0u, state->interval_max);

  std::optional<hci::Result<>> stop_result;
  this->advertiser()->StopAdvertising(
      [&stop_result](hci::Result<> result) { stop_result = result; });

  // Reconfigure with values that are within the range. These should get
  // passed down as is.
  const AdvertisingIntervalRange new_interval(
      hci_spec::kLEAdvertisingIntervalMin + 1,
      hci_spec::kLEAdvertisingIntervalMax - 1);
  AdvertisingOptions new_options(new_interval,
                                 kDefaultNoAdvFlags,
                                 /*init_extended_pdu=*/false,
                                 /*init_anonymous=*/false,
                                 /*init_include_tx_power_level=*/false);
  this->advertiser()->StartAdvertising(kRandomAddress,
                                       ad,
                                       scan_data,
                                       new_options,
                                       nullptr,
                                       this->MakeExpectSuccessCallback());
  this->RunUntilIdle();
  EXPECT_TRUE(this->TakeLastStatus());
  ASSERT_TRUE(stop_result.has_value());
  EXPECT_TRUE(stop_result->is_ok());

  state = this->GetControllerAdvertisingState();
  EXPECT_TRUE(state);
  EXPECT_EQ(new_interval.min(), state->interval_min);
  EXPECT_EQ(new_interval.max(), state->interval_max);
}

TYPED_TEST(LowEnergyAdvertiserTest, StartWhileStopping) {
  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();
  DeviceAddress addr = kRandomAddress;
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*init_extended_pdu=*/false,
                             /*init_anonymous=*/false,
                             /*init_include_tx_power_level=*/false);
  this->SetRandomAddress(addr);

  // Get to a started state.
  this->advertiser()->StartAdvertising(
      addr, ad, scan_data, options, nullptr, this->MakeExpectSuccessCallback());
  this->RunUntilIdle();
  std::optional<Result<AdvertisementId>> status = this->TakeLastStatus();
  ASSERT_TRUE(status);
  ASSERT_TRUE(status->is_ok());
  AdvertisementId adv_id = status->value();
  EXPECT_TRUE(this->GetControllerAdvertisingState().enabled);

  // Initiate a request to Stop and wait until it's partially in progress.
  bool enabled = true;
  bool was_disabled = false;
  auto adv_state_cb = [&] {
    enabled = this->GetControllerAdvertisingState().enabled;
    if (!was_disabled && !enabled) {
      was_disabled = true;

      // Starting now should cancel the stop sequence and succeed.
      this->advertiser()->StartAdvertising(addr,
                                           ad,
                                           scan_data,
                                           options,
                                           nullptr,
                                           this->MakeExpectSuccessCallback());
    }
  };
  this->test_device()->set_advertising_state_callback(adv_state_cb);

  std::optional<hci::Result<>> stop_result;
  this->advertiser()->StopAdvertising(
      adv_id, [&stop_result](hci::Result<> result) { stop_result = result; });

  // Advertising should have been momentarily disabled.
  this->RunUntilIdle();
  EXPECT_TRUE(was_disabled);
  EXPECT_TRUE(enabled);
  EXPECT_TRUE(this->GetControllerAdvertisingState().enabled);
  ASSERT_TRUE(stop_result.has_value());
  EXPECT_TRUE(stop_result->is_ok());

  this->test_device()->set_advertising_state_callback(nullptr);
}

TYPED_TEST(LowEnergyAdvertiserTest, StopWhileStarting) {
  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*extended_pdu=*/false,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/false);

  this->advertiser()->StartAdvertising(kPublicAddress,
                                       ad,
                                       scan_data,
                                       options,
                                       nullptr,
                                       this->MakeExpectErrorCallback());
  std::optional<hci::Result<>> stop_result;
  this->advertiser()->StopAdvertising(
      [&stop_result](hci::Result<> result) { stop_result = result; });

  this->RunUntilIdle();
  EXPECT_TRUE(this->TakeLastStatus());
  ASSERT_TRUE(stop_result.has_value());
  EXPECT_TRUE(stop_result->is_ok());
  EXPECT_FALSE(this->GetControllerAdvertisingState().enabled);
}

// - StopAdvertisement noops when the advertisement address is wrong
// - Sets the advertisement data to null when stopped to prevent data leakage
//   (re-enable advertising without changing data, intercept)
TYPED_TEST(LowEnergyAdvertiserTest, StopAdvertisingConditions) {
  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*extended_pdu=*/false,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/false);
  this->SetRandomAddress(kRandomAddress);

  this->advertiser()->StartAdvertising(kRandomAddress,
                                       ad,
                                       scan_data,
                                       options,
                                       nullptr,
                                       this->MakeExpectSuccessCallback());

  this->RunUntilIdle();
  std::optional<Result<AdvertisementId>> status = this->TakeLastStatus();
  ASSERT_TRUE(status);
  ASSERT_TRUE(status->is_ok());
  AdvertisementId adv_id = status->value();

  EXPECT_TRUE(this->GetControllerAdvertisingState().enabled);
  DynamicByteBuffer expected_ad(ad.CalculateBlockSize(/*include_flags=*/true));
  ad.WriteBlock(&expected_ad, kDefaultNoAdvFlags);
  EXPECT_TRUE(ContainersEqual(
      this->GetControllerAdvertisingState().advertised_view(), expected_ad));

  this->RunUntilIdle();
  AdvertisementId bad_id(0x0F);
  std::optional<hci::Result<>> stop_result;
  this->advertiser()->StopAdvertising(
      bad_id, [&stop_result](hci::Result<> result) { stop_result = result; });
  ASSERT_TRUE(stop_result.has_value());
  EXPECT_FALSE(stop_result->is_ok());
  EXPECT_TRUE(this->GetControllerAdvertisingState().enabled);
  EXPECT_TRUE(ContainersEqual(
      this->GetControllerAdvertisingState().advertised_view(), expected_ad));

  stop_result.reset();
  this->advertiser()->StopAdvertising(
      adv_id, [&stop_result](hci::Result<> result) { stop_result = result; });

  this->RunUntilIdle();
  ASSERT_TRUE(stop_result.has_value());
  EXPECT_TRUE(stop_result->is_ok());
  EXPECT_FALSE(this->GetControllerAdvertisingState().enabled);
  EXPECT_EQ(0u, this->GetControllerAdvertisingState().advertised_view().size());
  EXPECT_EQ(0u, this->GetControllerAdvertisingState().scan_rsp_view().size());
}

// Ensures advertising set data is removed from controller memory after
// advertising is stopped
TYPED_TEST(LowEnergyAdvertiserTest, StopAdvertisingSingleAdvertisement) {
  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();

  // start public address advertising
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*extended_pdu=*/false,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/false);
  this->advertiser()->StartAdvertising(kPublicAddress,
                                       ad,
                                       scan_data,
                                       options,
                                       nullptr,
                                       this->MakeExpectSuccessCallback());
  this->RunUntilIdle();
  std::optional<Result<AdvertisementId>> status = this->TakeLastStatus();
  ASSERT_TRUE(status);
  ASSERT_TRUE(status->is_ok());
  AdvertisementId adv_id = status->value();

  constexpr uint8_t blank[hci_spec::kMaxLEAdvertisingDataLength] = {0};

  // check that advertiser and controller both report the same advertising
  // state
  EXPECT_TRUE(this->advertiser()->IsAdvertising());
  EXPECT_TRUE(this->advertiser()->IsAdvertising(adv_id));

  {
    const FakeController::LEAdvertisingState& state =
        this->GetControllerAdvertisingState();
    EXPECT_TRUE(state.enabled);
    EXPECT_NE(
        0,
        std::memcmp(blank, state.data, hci_spec::kMaxLEAdvertisingDataLength));
    EXPECT_NE(0, state.data_length);
    EXPECT_NE(
        0,
        std::memcmp(blank, state.data, hci_spec::kMaxLEAdvertisingDataLength));
    EXPECT_NE(0, state.scan_rsp_length);
  }

  // stop advertising the random address
  std::optional<hci::Result<>> stop_result;
  this->advertiser()->StopAdvertising(
      adv_id, [&stop_result](hci::Result<> result) { stop_result = result; });
  this->RunUntilIdle();

  // check that advertiser and controller both report the same advertising
  // state
  ASSERT_TRUE(stop_result.has_value());
  EXPECT_TRUE(stop_result->is_ok());
  EXPECT_FALSE(this->advertiser()->IsAdvertising());
  EXPECT_FALSE(this->advertiser()->IsAdvertising(adv_id));

  {
    const FakeController::LEAdvertisingState& state =
        this->GetControllerAdvertisingState();
    EXPECT_FALSE(state.enabled);
    EXPECT_EQ(
        0,
        std::memcmp(blank, state.data, hci_spec::kMaxLEAdvertisingDataLength));
    EXPECT_EQ(0, state.data_length);
    EXPECT_EQ(
        0,
        std::memcmp(blank, state.data, hci_spec::kMaxLEAdvertisingDataLength));
    EXPECT_EQ(0, state.scan_rsp_length);
  }
}

// - Rejects anonymous advertisement (unsupported)
TYPED_TEST(LowEnergyAdvertiserTest, NoAnonymous) {
  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*extended_pdu=*/false,
                             /*anonymous=*/true,
                             /*include_tx_power_level=*/false);
  this->SetRandomAddress(kRandomAddress);

  this->advertiser()->StartAdvertising(kRandomAddress,
                                       ad,
                                       scan_data,
                                       options,
                                       nullptr,
                                       this->MakeExpectErrorCallback());
  EXPECT_TRUE(this->TakeLastStatus());
  EXPECT_FALSE(this->GetControllerAdvertisingState().enabled);
}

TYPED_TEST(LowEnergyAdvertiserTest, AdvertisingDataTooLong) {
  AdvertisingData invalid_ad =
      this->GetTooLargeExampleData(/*include_flags=*/true);
  AdvertisingData valid_scan_rsp =
      this->GetExampleData(/*include_flags=*/false);
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*extended_pdu=*/false,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/false);
  this->SetRandomAddress(kRandomAddress);

  // Advertising data too large.
  this->advertiser()->StartAdvertising(kRandomAddress,
                                       invalid_ad,
                                       valid_scan_rsp,
                                       options,
                                       nullptr,
                                       this->MakeExpectErrorCallback());
  this->RunUntilIdle();
  auto status = this->TakeLastStatus();
  ASSERT_TRUE(status);
  ASSERT_TRUE(status->is_error());
  EXPECT_TRUE(status->error_value().is(HostError::kAdvertisingDataTooLong));
}

TYPED_TEST(LowEnergyAdvertiserTest, AdvertisingDataTooLongWithTxPower) {
  AdvertisingData invalid_ad = this->GetTooLargeExampleData(
      /*include_flags=*/true,
      hci_spec::kMaxLEAdvertisingDataLength - kTLVTxPowerLevelSize + 1);
  AdvertisingData valid_scan_rsp =
      this->GetExampleData(/*include_flags=*/false);
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*extended_pdu=*/false,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/true);
  this->SetRandomAddress(kRandomAddress);

  // Advertising data too large.
  this->advertiser()->StartAdvertising(kRandomAddress,
                                       invalid_ad,
                                       valid_scan_rsp,
                                       options,
                                       nullptr,
                                       this->MakeExpectErrorCallback());
  this->RunUntilIdle();
  auto status = this->TakeLastStatus();
  ASSERT_TRUE(status);
  ASSERT_TRUE(status->is_error());
  EXPECT_TRUE(status->error_value().is(HostError::kAdvertisingDataTooLong));
}

TYPED_TEST(LowEnergyAdvertiserTest, ScanResponseTooLong) {
  AdvertisingData valid_ad = this->GetExampleData();
  AdvertisingData invalid_scan_rsp =
      this->GetTooLargeExampleData(/*include_flags=*/false);
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*extended_pdu=*/false,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/false);
  this->SetRandomAddress(kRandomAddress);

  this->advertiser()->StartAdvertising(kRandomAddress,
                                       valid_ad,
                                       invalid_scan_rsp,
                                       options,
                                       nullptr,
                                       this->MakeExpectErrorCallback());
  this->RunUntilIdle();
  auto status = this->TakeLastStatus();
  ASSERT_TRUE(status);
  ASSERT_TRUE(status->is_error());
  EXPECT_TRUE(status->error_value().is(HostError::kScanResponseTooLong));
}

TYPED_TEST(LowEnergyAdvertiserTest, ScanResponseTooLongWithTxPower) {
  AdvertisingData valid_ad = this->GetExampleData();
  AdvertisingData invalid_scan_rsp = this->GetTooLargeExampleData(
      /*include_flags=*/false,
      hci_spec::kMaxLEAdvertisingDataLength - kTLVTxPowerLevelSize + 1);
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*extended_pdu=*/false,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/true);
  this->SetRandomAddress(kRandomAddress);

  this->advertiser()->StartAdvertising(kRandomAddress,
                                       valid_ad,
                                       invalid_scan_rsp,
                                       options,
                                       nullptr,
                                       this->MakeExpectErrorCallback());
  this->RunUntilIdle();
  auto status = this->TakeLastStatus();
  ASSERT_TRUE(status);
  ASSERT_TRUE(status->is_error());
  EXPECT_TRUE(status->error_value().is(HostError::kScanResponseTooLong));
}

TYPED_TEST(LowEnergyAdvertiserTest,
           DestroyingTransportBeforeAdvertiserDoesNotCrash) {
  this->DeleteTransport();
  this->DestroyAdvertiser();
}

// Ensure we disallow requesting extended PDUs where we can't use them
TYPED_TEST(LowEnergyAdvertiserTest, ExtendedPdusReturnsErrorWhenNotSupported) {
  if (this->SupportsExtendedPdus()) {
    return;
  }

  AdvertisingData ad = this->GetExampleData();
  AdvertisingData scan_data = this->GetExampleData();
  AdvertisingOptions options(kTestInterval,
                             kDefaultNoAdvFlags,
                             /*extended_pdu=*/true,
                             /*anonymous=*/false,
                             /*include_tx_power_level=*/false);
  this->SetRandomAddress(kRandomAddress);

  this->advertiser()->StartAdvertising(kRandomAddress,
                                       ad,
                                       scan_data,
                                       options,
                                       nullptr,
                                       this->MakeExpectErrorCallback());
  this->RunUntilIdle();
  auto status = this->TakeLastStatus();
  ASSERT_TRUE(status);
  ASSERT_TRUE(status->is_error());
  EXPECT_TRUE(status->error_value().is(HostError::kNotSupported));
}

}  // namespace
}  // namespace bt::hci
