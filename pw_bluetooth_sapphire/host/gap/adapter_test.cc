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

#include "pw_bluetooth_sapphire/internal/host/gap/adapter.h"

#include <memory>

#include "pw_bluetooth_sapphire/internal/host/gap/bredr_discovery_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_address_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_advertising_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_discovery_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/fake_layer.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/util.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/fake_l2cap.h"
#include "pw_bluetooth_sapphire/internal/host/testing/controller_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_controller.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_peer.h"
#include "pw_bluetooth_sapphire/internal/host/testing/inspect.h"

namespace bt::gap {
namespace {

using namespace inspect::testing;
namespace android_hci = hci_spec::vendor::android;
using testing::FakeController;
using testing::FakePeer;
using TestingBase = testing::FakeDispatcherControllerTest<FakeController>;

using FeaturesBits = pw::bluetooth::Controller::FeaturesBits;

const DeviceAddress kTestAddr(DeviceAddress::Type::kLEPublic,
                              {0x01, 0, 0, 0, 0, 0});
const DeviceAddress kTestAddr2(DeviceAddress::Type::kLEPublic,
                               {2, 0, 0, 0, 0, 0});
const DeviceAddress kTestAddrBrEdr(DeviceAddress::Type::kBREDR,
                                   {3, 0, 0, 0, 0, 0});

constexpr FeaturesBits kDefaultFeaturesBits =
    FeaturesBits::kHciSco | FeaturesBits::kHciIso |
    FeaturesBits::kSetAclPriorityCommand;

class AdapterTest : public TestingBase {
 public:
  AdapterTest() = default;
  ~AdapterTest() override = default;

  void SetUp() override { SetUp(kDefaultFeaturesBits); }

  void SetUp(FeaturesBits features) {
    // Don't initialize Transport yet because Adapter initializes Transport.
    TestingBase::SetUp(features, /*initialize_transport=*/false);

    transport_closed_called_ = false;

    auto l2cap = std::make_unique<l2cap::testing::FakeL2cap>(dispatcher());
    gatt_ = std::make_unique<gatt::testing::FakeLayer>(dispatcher());
    Adapter::Config config = {
        .legacy_pairing_enabled = false,
    };
    adapter_ = Adapter::Create(dispatcher(),
                               transport()->GetWeakPtr(),
                               gatt_->GetWeakPtr(),
                               config,
                               lease_provider_,
                               std::move(l2cap));
  }

  void TearDown() override {
    if (adapter_->IsInitialized()) {
      adapter_->ShutDown();
    }

    adapter_ = nullptr;
    gatt_ = nullptr;
    TestingBase::TearDown();
  }

  void InitializeAdapter(Adapter::InitializeCallback callback) {
    adapter_->Initialize(std::move(callback),
                         [this] { transport_closed_called_ = true; });
    RunUntilIdle();
  }

  bool EnsureInitialized() {
    bool initialized = false;
    InitializeAdapter([&](bool success) {
      EXPECT_TRUE(success);
      initialized = true;
    });
    return initialized;
  }

 protected:
  void GetSupportedDelayRangeHelper(
      bool supported, const std::optional<std::vector<uint8_t>>& codec_config);

  bool transport_closed_called() const { return transport_closed_called_; }

  Adapter* adapter() const { return adapter_.get(); }

 private:
  bool transport_closed_called_;
  pw::bluetooth_sapphire::testing::FakeLeaseProvider lease_provider_;
  std::unique_ptr<gatt::testing::FakeLayer> gatt_;
  std::unique_ptr<Adapter> adapter_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(AdapterTest);
};

class AdapterScoAndIsoDisabledTest : public AdapterTest {
 public:
  void SetUp() override { AdapterTest::SetUp(FeaturesBits{0}); }
};

TEST_F(AdapterTest, InitializeFailureNoFeaturesSupported) {
  bool success;
  int init_cb_count = 0;
  auto init_cb = [&](bool cb_success) {
    success = cb_success;
    init_cb_count++;
  };

  // The controller supports nothing.
  InitializeAdapter(std::move(init_cb));
  EXPECT_FALSE(success);
  EXPECT_EQ(1, init_cb_count);
  EXPECT_FALSE(transport_closed_called());
}

TEST_F(AdapterTest, InitializeFailureNoBufferInfo) {
  bool success;
  int init_cb_count = 0;
  auto init_cb = [&](bool cb_success) {
    success = cb_success;
    init_cb_count++;
  };

  // Enable LE support.
  FakeController::Settings settings;
  settings.lmp_features_page0 |=
      static_cast<uint64_t>(hci_spec::LMPFeature::kLESupportedHost);
  test_device()->set_settings(settings);

  InitializeAdapter(std::move(init_cb));
  EXPECT_FALSE(success);
  EXPECT_EQ(1, init_cb_count);
  EXPECT_FALSE(transport_closed_called());
}

TEST_F(AdapterTest, InitializeNoBREDR) {
  bool success;
  int init_cb_count = 0;
  auto init_cb = [&](bool cb_success) {
    success = cb_success;
    init_cb_count++;
  };

  // Enable LE support, disable BR/EDR
  FakeController::Settings settings;
  settings.lmp_features_page0 |=
      static_cast<uint64_t>(hci_spec::LMPFeature::kLESupportedHost);
  settings.lmp_features_page0 |=
      static_cast<uint64_t>(hci_spec::LMPFeature::kBREDRNotSupported);
  settings.le_acl_data_packet_length = 0x1B;
  settings.le_total_num_acl_data_packets = 1;
  test_device()->set_settings(settings);

  InitializeAdapter(std::move(init_cb));
  EXPECT_TRUE(success);
  EXPECT_EQ(1, init_cb_count);
  EXPECT_TRUE(adapter()->state().IsLowEnergySupported());
  EXPECT_FALSE(adapter()->state().IsBREDRSupported());
  EXPECT_FALSE(adapter()->bredr());
  EXPECT_EQ(TechnologyType::kLowEnergy, adapter()->state().type());
  EXPECT_FALSE(transport_closed_called());
}

TEST_F(AdapterTest, InitializeQueriesAndroidExtensionsCapabilitiesIfSupported) {
  TearDown();
  SetUp(FeaturesBits::kAndroidVendorExtensions);

  bool success;
  int init_cb_count = 0;
  auto init_cb = [&](bool cb_success) {
    success = cb_success;
    init_cb_count++;
  };

  FakeController::Settings settings;
  settings.ApplyLEOnlyDefaults();
  settings.ApplyAndroidVendorExtensionDefaults();
  test_device()->set_settings(settings);

  InitializeAdapter(std::move(init_cb));
  EXPECT_TRUE(success);
  EXPECT_EQ(1, init_cb_count);
  EXPECT_TRUE(adapter()->state().android_vendor_capabilities.has_value());
}

TEST_F(AdapterTest,
       InitializeQueryAndroidExtensionsCapabilitiesFailureHandled) {
  TearDown();
  SetUp(FeaturesBits::kAndroidVendorExtensions);

  bool success;
  int init_cb_count = 0;
  auto init_cb = [&](bool cb_success) {
    success = cb_success;
    init_cb_count++;
  };

  FakeController::Settings settings;
  settings.ApplyLEOnlyDefaults();
  settings.ApplyAndroidVendorExtensionDefaults();
  test_device()->set_settings(settings);

  test_device()->SetDefaultResponseStatus(
      android_hci::kLEGetVendorCapabilities,
      pw::bluetooth::emboss::StatusCode::COMMAND_DISALLOWED);
  InitializeAdapter(std::move(init_cb));
  EXPECT_FALSE(success);
  EXPECT_EQ(1, init_cb_count);
  EXPECT_FALSE(adapter()->state().android_vendor_capabilities.has_value());
}

TEST_F(AdapterTest, InitializeSuccess) {
  bool success;
  int init_cb_count = 0;
  auto init_cb = [&](bool cb_success) {
    success = cb_success;
    init_cb_count++;
  };

  // Return valid buffer information and enable LE support. (This should
  // succeed).
  FakeController::Settings settings;
  settings.lmp_features_page0 |=
      static_cast<uint64_t>(hci_spec::LMPFeature::kLESupportedHost);
  settings.le_acl_data_packet_length = 0x1B;
  settings.le_total_num_acl_data_packets = 1;
  test_device()->set_settings(settings);

  InitializeAdapter(std::move(init_cb));
  EXPECT_TRUE(success);
  EXPECT_EQ(1, init_cb_count);
  EXPECT_TRUE(adapter()->state().IsLowEnergySupported());
  EXPECT_TRUE(adapter()->state().IsBREDRSupported());
  EXPECT_TRUE(adapter()->le());
  EXPECT_TRUE(adapter()->bredr());
  EXPECT_EQ(TechnologyType::kDualMode, adapter()->state().type());
  EXPECT_FALSE(transport_closed_called());
}

TEST_F(AdapterTest, InitializeFailureHCICommandError) {
  bool success;
  int init_cb_count = 0;
  auto init_cb = [&](bool cb_success) {
    success = cb_success;
    init_cb_count++;
  };

  // Make all settings valid but make an HCI command fail.
  FakeController::Settings settings;
  settings.ApplyLEOnlyDefaults();
  test_device()->set_settings(settings);
  test_device()->SetDefaultResponseStatus(
      hci_spec::kLEReadLocalSupportedFeatures,
      pw::bluetooth::emboss::StatusCode::HARDWARE_FAILURE);

  InitializeAdapter(std::move(init_cb));
  EXPECT_FALSE(success);
  EXPECT_EQ(1, init_cb_count);
  EXPECT_FALSE(adapter()->state().IsLowEnergySupported());
  EXPECT_FALSE(transport_closed_called());
}

TEST_F(AdapterTest, InitializeFailureTransportErrorDuringWriteLocalName) {
  std::optional<bool> success;
  int init_cb_count = 0;
  auto init_cb = [&](bool cb_success) {
    success = cb_success;
    init_cb_count++;
  };

  // Make all settings valid but make an HCI command fail.
  FakeController::Settings settings;
  settings.ApplyDualModeDefaults();
  test_device()->set_settings(settings);
  fit::closure resume_write_local_name_cb = nullptr;
  test_device()->pause_responses_for_opcode(
      hci_spec::kWriteLocalName, [&](fit::closure resume) {
        resume_write_local_name_cb = std::move(resume);
      });

  InitializeAdapter(std::move(init_cb));
  ASSERT_TRUE(resume_write_local_name_cb);
  EXPECT_EQ(0, init_cb_count);

  // Signaling an error should cause Transport to close, which should cause
  // initialization to fail.
  test_device()->SignalError(pw::Status::Unknown());
  ASSERT_TRUE(success.has_value());
  EXPECT_FALSE(*success);
  EXPECT_EQ(1, init_cb_count);
  EXPECT_FALSE(transport_closed_called());
}

TEST_F(AdapterTest, TransportClosedCallback) {
  bool success;
  int init_cb_count = 0;
  auto init_cb = [&](bool cb_success) {
    success = cb_success;
    init_cb_count++;
  };

  FakeController::Settings settings;
  settings.ApplyLEOnlyDefaults();
  test_device()->set_settings(settings);

  InitializeAdapter(std::move(init_cb));
  EXPECT_TRUE(success);
  EXPECT_EQ(1, init_cb_count);
  EXPECT_TRUE(adapter()->state().IsLowEnergySupported());
  EXPECT_FALSE(transport_closed_called());

  test_device()->SignalError(pw::Status::Aborted());
  RunUntilIdle();

  EXPECT_TRUE(transport_closed_called());
  EXPECT_EQ(1, init_cb_count);
}

// TODO(fxbug.dev/42088281): Add a unit test for Adapter::ShutDown() and update
// ShutDownDuringInitialize() with the same expectations.

TEST_F(AdapterTest, ShutDownDuringInitialize) {
  bool success;
  int init_cb_count = 0;
  auto init_cb = [&](bool result) {
    success = result;
    init_cb_count++;
  };

  FakeController::Settings settings;
  settings.ApplyLEOnlyDefaults();
  test_device()->set_settings(settings);

  adapter()->Initialize(std::move(init_cb), [] {});
  EXPECT_TRUE(adapter()->IsInitializing());
  adapter()->ShutDown();

  EXPECT_EQ(1, init_cb_count);
  EXPECT_FALSE(success);
  EXPECT_FALSE(adapter()->IsInitializing());
  EXPECT_FALSE(adapter()->IsInitialized());

  // Further calls to ShutDown() should have no effect.
  adapter()->ShutDown();
  RunUntilIdle();
}

TEST_F(AdapterTest, SetNameError) {
  std::string kNewName = "something";

  // Make all settings valid but make WriteLocalName command fail.
  FakeController::Settings settings;
  settings.ApplyDualModeDefaults();
  test_device()->set_settings(settings);
  test_device()->SetDefaultResponseStatus(
      hci_spec::kWriteLocalName,
      pw::bluetooth::emboss::StatusCode::HARDWARE_FAILURE);
  ASSERT_TRUE(EnsureInitialized());

  hci::Result<> result = fit::ok();
  auto name_cb = [&result](const auto& status) { result = status; };

  adapter()->SetLocalName(kNewName, name_cb);

  RunUntilIdle();

  EXPECT_EQ(ToResult(pw::bluetooth::emboss::StatusCode::HARDWARE_FAILURE),
            result);
}

TEST_F(AdapterTest, SetNameSuccess) {
  const std::string kNewName = "Fuchsia BT 💖✨";

  FakeController::Settings settings;
  settings.ApplyDualModeDefaults();
  test_device()->set_settings(settings);
  ASSERT_TRUE(EnsureInitialized());

  hci::Result<> result = ToResult(HostError::kFailed);
  auto name_cb = [&result](const auto& status) { result = status; };
  adapter()->SetLocalName(kNewName, name_cb);

  RunUntilIdle();

  EXPECT_EQ(fit::ok(), result);
  EXPECT_EQ(kNewName, test_device()->local_name());
}

// Tests that writing a local name that is larger than the maximum size
// succeeds. The saved local name is the original (untruncated) local name.
TEST_F(AdapterTest, SetNameLargerThanMax) {
  const std::string long_name(hci_spec::kMaxNameLength + 1, 'x');

  FakeController::Settings settings;
  settings.ApplyDualModeDefaults();
  test_device()->set_settings(settings);
  ASSERT_TRUE(EnsureInitialized());

  hci::Result<> result = ToResult(HostError::kFailed);
  auto name_cb = [&result](const auto& status) { result = status; };
  adapter()->SetLocalName(long_name, name_cb);

  RunUntilIdle();

  EXPECT_EQ(fit::ok(), result);
  EXPECT_EQ(long_name, adapter()->state().local_name);
}

// Tests that SetLocalName results in BrEdrDiscoveryManager updating it's local
// name.
TEST_F(AdapterTest, SetLocalNameCallsBrEdrUpdateLocalName) {
  const std::string kNewName = "This is a test BT name! 1234";

  FakeController::Settings settings;
  settings.ApplyDualModeDefaults();
  test_device()->set_settings(settings);
  ASSERT_TRUE(EnsureInitialized());
  ASSERT_TRUE(adapter()->bredr());

  hci::Result<> result = ToResult(HostError::kFailed);
  auto name_cb = [&result](const auto& status) { result = status; };
  adapter()->SetLocalName(kNewName, name_cb);

  RunUntilIdle();

  EXPECT_EQ(fit::ok(), result);
  EXPECT_EQ(kNewName, adapter()->state().local_name);
  EXPECT_EQ(kNewName, adapter()->local_name());
}

// Tests that writing a long local name results in BrEdr updating it's local
// name. Should still succeed, and the stored local name should be the original
// name.
TEST_F(AdapterTest, BrEdrUpdateLocalNameLargerThanMax) {
  const std::string long_name(
      hci_spec::kExtendedInquiryResponseMaxNameBytes + 2, 'x');

  FakeController::Settings settings;
  settings.ApplyDualModeDefaults();
  test_device()->set_settings(settings);
  ASSERT_TRUE(EnsureInitialized());
  EXPECT_TRUE(adapter()->bredr());

  hci::Result<> result = ToResult(HostError::kFailed);
  auto name_cb = [&result](const auto& status) { result = status; };
  adapter()->SetLocalName(long_name, name_cb);

  RunUntilIdle();

  EXPECT_EQ(fit::ok(), result);
  // Both the adapter & discovery manager local name should be the original
  // (untruncated) name.
  EXPECT_EQ(long_name, adapter()->state().local_name);
  EXPECT_EQ(long_name, adapter()->local_name());
}

// Tests WriteExtendedInquiryResponse failure leads to |local_name_| not
// updated.
TEST_F(AdapterTest, BrEdrUpdateEIRResponseError) {
  std::string kNewName = "EirFailure";

  // Make all settings valid but make WriteExtendedInquiryResponse command fail.
  FakeController::Settings settings;
  settings.ApplyDualModeDefaults();
  test_device()->set_settings(settings);
  test_device()->SetDefaultResponseStatus(
      hci_spec::kWriteExtendedInquiryResponse,
      pw::bluetooth::emboss::StatusCode::CONNECTION_TERMINATED_BY_LOCAL_HOST);
  ASSERT_TRUE(EnsureInitialized());

  hci::Result<> result = fit::ok();
  auto name_cb = [&result](const auto& status) { result = status; };

  adapter()->SetLocalName(kNewName, name_cb);

  RunUntilIdle();

  // kWriteLocalName will succeed, but kWriteExtendedInquiryResponse will fail
  EXPECT_EQ(ToResult(pw::bluetooth::emboss::StatusCode::
                         CONNECTION_TERMINATED_BY_LOCAL_HOST),
            result);
  // The |local_name_| should not be set.
  EXPECT_NE(kNewName, adapter()->state().local_name);
  EXPECT_NE(kNewName, adapter()->local_name());
}

TEST_F(AdapterTest, DefaultName) {
  FakeController::Settings settings;
  settings.ApplyDualModeDefaults();
  test_device()->set_settings(settings);

  bool initialized = false;
  InitializeAdapter([&](bool success) {
    // Ensure that the local name has been written to the controller when
    // initialization has completed.
    EXPECT_TRUE(success);
    EXPECT_EQ(kDefaultLocalName, test_device()->local_name());
    EXPECT_EQ(kDefaultLocalName, adapter()->state().local_name);

    initialized = true;
  });

  EXPECT_TRUE(initialized);
}

TEST_F(AdapterTest, PeerCacheReturnsNonNull) {
  EXPECT_TRUE(adapter()->peer_cache());
}

TEST_F(AdapterTest, LeAutoConnect) {
  constexpr pw::chrono::SystemClock::duration kTestScanPeriod =
      std::chrono::seconds(10);
  constexpr PeerId kPeerId(1234);

  FakeController::Settings settings;
  settings.ApplyLEOnlyDefaults();
  test_device()->set_settings(settings);

  InitializeAdapter([](bool) {});
  adapter()->le()->set_scan_period_for_testing(kTestScanPeriod);

  auto fake_peer =
      std::make_unique<FakePeer>(kTestAddr, dispatcher(), true, false);
  fake_peer->set_directed_advertising_enabled(true);
  test_device()->AddPeer(std::move(fake_peer));

  std::unique_ptr<bt::gap::LowEnergyConnectionHandle> conn;
  adapter()->set_auto_connect_callback(
      [&](auto conn_ref) { conn = std::move(conn_ref); });

  // Enable background scanning. No auto-connect should take place since the
  // device isn't yet bonded.
  std::unique_ptr<LowEnergyDiscoverySession> session;
  adapter()->le()->StartDiscovery(
      /*active=*/false, {}, [&session](auto cb_session) {
        session = std::move(cb_session);
      });
  RunUntilIdle();
  EXPECT_FALSE(conn);
  EXPECT_EQ(0u, adapter()->peer_cache()->count());

  // Mark the peer as bonded and advance the scan period.
  sm::PairingData pdata;
  pdata.peer_ltk = sm::LTK();
  pdata.local_ltk = sm::LTK();
  adapter()->peer_cache()->AddBondedPeer(
      BondingData{.identifier = kPeerId,
                  .address = kTestAddr,
                  .name = std::nullopt,
                  .device_class = {},
                  .le_pairing_data = pdata,
                  .bredr_link_key = std::nullopt,
                  .bredr_services = {}});
  EXPECT_EQ(1u, adapter()->peer_cache()->count());

  // FakeController only sends advertising reports at the start of scan periods,
  // so we need to start a second period.
  RunFor(kTestScanPeriod);

  // The peer should have been auto-connected.
  ASSERT_TRUE(conn);
  EXPECT_EQ(kPeerId, conn->peer_identifier());
}

TEST_F(AdapterTest, LeSkipAutoConnectBehavior) {
  constexpr pw::chrono::SystemClock::duration kTestScanPeriod =
      std::chrono::seconds(10);
  constexpr PeerId kPeerId(1234);

  FakeController::Settings settings;
  settings.ApplyLEOnlyDefaults();
  test_device()->set_settings(settings);

  InitializeAdapter([](bool) {});
  adapter()->le()->set_scan_period_for_testing(kTestScanPeriod);

  auto fake_peer =
      std::make_unique<FakePeer>(kTestAddr, dispatcher(), true, false);
  fake_peer->set_directed_advertising_enabled(true);
  test_device()->AddPeer(std::move(fake_peer));

  std::unique_ptr<bt::gap::LowEnergyConnectionHandle> conn;
  adapter()->set_auto_connect_callback(
      [&](auto conn_ref) { conn = std::move(conn_ref); });

  // Enable background scanning. No auto-connect should take place since the
  // device isn't yet bonded.
  std::unique_ptr<LowEnergyDiscoverySession> session;
  adapter()->le()->StartDiscovery(
      /*active=*/false, {}, [&session](auto cb_session) {
        session = std::move(cb_session);
      });
  RunUntilIdle();
  EXPECT_FALSE(conn);
  EXPECT_EQ(0u, adapter()->peer_cache()->count());

  // Mark the peer as bonded.
  sm::PairingData pdata;
  pdata.peer_ltk = sm::LTK();
  pdata.local_ltk = sm::LTK();
  adapter()->peer_cache()->AddBondedPeer(
      BondingData{.identifier = kPeerId,
                  .address = kTestAddr,
                  .name = std::nullopt,
                  .device_class = {},
                  .le_pairing_data = pdata,
                  .bredr_link_key = std::nullopt,
                  .bredr_services = {}});
  EXPECT_EQ(1u, adapter()->peer_cache()->count());

  // Fake a manual disconnect to skip auto-connect behavior.
  adapter()->peer_cache()->SetAutoConnectBehaviorForIntentionalDisconnect(
      kPeerId);

  // Advance the scan period.
  RunFor(kTestScanPeriod);

  // The peer should NOT have been auto-connected.
  ASSERT_FALSE(conn);

  // The peer should still not auto-connect after a subsequent scan period.
  RunFor(kTestScanPeriod);
  ASSERT_FALSE(conn);

  // Fake a manual connection to reset auto-connect behavior.
  adapter()->peer_cache()->SetAutoConnectBehaviorForSuccessfulConnection(
      kPeerId);

  // Advance the scan period.
  RunFor(kTestScanPeriod);

  // The peer SHOULD have been auto-connected.
  ASSERT_TRUE(conn);
  EXPECT_EQ(kPeerId, conn->peer_identifier());
}

// Tests the interactions between the advertising manager and the local address
// manager when the controller uses legacy advertising.
TEST_F(AdapterTest, LocalAddressForLegacyAdvertising) {
  FakeController::Settings settings;
  settings.ApplyLegacyLEConfig();
  test_device()->set_settings(settings);
  InitializeAdapter([](bool) {});

  std::optional<AdvertisementInstance> instance;
  auto adv_cb = [&](auto i, hci::Result<> status) {
    instance = std::move(i);
    EXPECT_EQ(fit::ok(), status);
  };

  // Advertising should use the public address by default when privacy is not
  // enabled.
  adapter()->le()->StartAdvertising(AdvertisingData(),
                                    AdvertisingData(),
                                    AdvertisingInterval::FAST1,
                                    /*extended_pdu=*/false,
                                    /*anonymous=*/false,
                                    /*include_tx_power_level=*/false,
                                    /*connectable=*/std::nullopt,
                                    /*address_type=*/std::nullopt,
                                    adv_cb);
  RunUntilIdle();
  EXPECT_TRUE(test_device()->legacy_advertising_state().enabled);
  EXPECT_EQ(pw::bluetooth::emboss::LEOwnAddressType::PUBLIC,
            test_device()->legacy_advertising_state().own_address_type);

  // Enable privacy. The random address should not get configured while
  // advertising is in progress.
  adapter()->le()->EnablePrivacy(true);
  RunUntilIdle();
  EXPECT_FALSE(test_device()->legacy_advertising_state().random_address);

  // Stop advertising.
  instance.reset();
  RunUntilIdle();
  EXPECT_FALSE(test_device()->legacy_advertising_state().enabled);
  EXPECT_FALSE(test_device()->legacy_advertising_state().random_address);

  // Restart advertising. This should configure the LE random address and
  // advertise using it.
  adapter()->le()->StartAdvertising(AdvertisingData(),
                                    AdvertisingData(),
                                    AdvertisingInterval::FAST1,
                                    /*extended_pdu=*/false,
                                    /*anonymous=*/false,
                                    /*include_tx_power_level=*/false,
                                    /*connectable=*/std::nullopt,
                                    /*address_type=*/std::nullopt,
                                    adv_cb);
  RunUntilIdle();
  EXPECT_TRUE(test_device()->legacy_advertising_state().random_address);
  EXPECT_TRUE(test_device()->legacy_advertising_state().enabled);
  EXPECT_EQ(pw::bluetooth::emboss::LEOwnAddressType::RANDOM,
            test_device()->legacy_advertising_state().own_address_type);

  // Advance time to force the random address to refresh. The update should be
  // deferred while advertising.
  auto last_random_addr =
      *test_device()->legacy_advertising_state().random_address;
  RunFor(kPrivateAddressTimeout);
  EXPECT_EQ(last_random_addr,
            *test_device()->legacy_advertising_state().random_address);

  // Restarting advertising should refresh the controller address.
  instance.reset();
  adapter()->le()->StartAdvertising(AdvertisingData(),
                                    AdvertisingData(),
                                    AdvertisingInterval::FAST1,
                                    /*extended_pdu=*/false,
                                    /*anonymous=*/false,
                                    /*include_tx_power_level=*/false,
                                    /*connectable=*/std::nullopt,
                                    /*address_type=*/std::nullopt,
                                    adv_cb);
  RunUntilIdle();
  EXPECT_TRUE(test_device()->legacy_advertising_state().enabled);
  EXPECT_EQ(pw::bluetooth::emboss::LEOwnAddressType::RANDOM,
            test_device()->legacy_advertising_state().own_address_type);
  EXPECT_TRUE(test_device()->legacy_advertising_state().random_address);
  EXPECT_NE(last_random_addr,
            test_device()->legacy_advertising_state().random_address);

  // Disable privacy. The next time advertising gets started it should use a
  // public address.
  adapter()->le()->EnablePrivacy(false);
  instance.reset();
  adapter()->le()->StartAdvertising(AdvertisingData(),
                                    AdvertisingData(),
                                    AdvertisingInterval::FAST1,
                                    /*extended_pdu=*/false,
                                    /*anonymous=*/false,
                                    /*include_tx_power_level=*/false,
                                    /*connectable=*/std::nullopt,
                                    /*address_type=*/std::nullopt,
                                    adv_cb);
  RunUntilIdle();
  EXPECT_TRUE(test_device()->legacy_advertising_state().enabled);
  EXPECT_EQ(pw::bluetooth::emboss::LEOwnAddressType::PUBLIC,
            test_device()->legacy_advertising_state().own_address_type);
}

// Tests the interactions between the discovery manager and the local address
// manager.
TEST_F(AdapterTest, LocalAddressForDiscovery) {
  FakeController::Settings settings;
  settings.ApplyLegacyLEConfig();
  test_device()->set_settings(settings);
  InitializeAdapter([](bool) {});

  // Set a scan period that is longer than the private address timeout, for
  // testing.
  constexpr pw::chrono::SystemClock::duration kTestDelay =
      std::chrono::seconds(5);
  constexpr pw::chrono::SystemClock::duration kTestScanPeriod =
      kPrivateAddressTimeout + kTestDelay;
  adapter()->le()->set_scan_period_for_testing(kTestScanPeriod);

  // Discovery should use the public address by default.
  LowEnergyDiscoverySessionPtr session;
  auto cb = [&](auto s) { session = std::move(s); };
  adapter()->le()->StartDiscovery(/*active=*/true, {}, cb);
  RunUntilIdle();
  ASSERT_TRUE(session);
  EXPECT_TRUE(test_device()->le_scan_state().enabled);
  EXPECT_EQ(pw::bluetooth::emboss::LEOwnAddressType::PUBLIC,
            test_device()->le_scan_state().own_address_type);

  // Enable privacy. The random address should not get configured while a scan
  // is in progress.
  adapter()->le()->EnablePrivacy(true);
  RunUntilIdle();
  EXPECT_FALSE(test_device()->legacy_advertising_state().random_address);

  // Stop discovery.
  session = nullptr;
  RunUntilIdle();
  EXPECT_FALSE(test_device()->le_scan_state().enabled);
  EXPECT_FALSE(test_device()->legacy_advertising_state().random_address);

  // Restart discovery. This should configure the LE random address and scan
  // using it.
  adapter()->le()->StartDiscovery(/*active=*/true, {}, cb);
  RunUntilIdle();
  ASSERT_TRUE(session);
  EXPECT_TRUE(test_device()->le_scan_state().enabled);
  EXPECT_EQ(pw::bluetooth::emboss::LEOwnAddressType::RANDOM,
            test_device()->le_scan_state().own_address_type);

  // Advance time to force the random address to refresh. The update should be
  // deferred while still scanning.
  ASSERT_TRUE(test_device()->legacy_advertising_state().random_address);
  auto last_random_addr =
      *test_device()->legacy_advertising_state().random_address;
  RunFor(kPrivateAddressTimeout);
  EXPECT_EQ(last_random_addr,
            *test_device()->legacy_advertising_state().random_address);

  // Let the scan period expire. This should restart scanning and refresh the
  // random address.
  RunFor(kTestDelay);
  EXPECT_TRUE(test_device()->le_scan_state().enabled);
  EXPECT_EQ(pw::bluetooth::emboss::LEOwnAddressType::RANDOM,
            test_device()->le_scan_state().own_address_type);
  ASSERT_TRUE(test_device()->legacy_advertising_state().random_address);
  EXPECT_NE(last_random_addr,
            test_device()->legacy_advertising_state().random_address);

  // Disable privacy. The next time scanning gets started it should use a
  // public address.
  adapter()->le()->EnablePrivacy(false);
  RunFor(kTestScanPeriod);
  EXPECT_TRUE(test_device()->le_scan_state().enabled);
  EXPECT_EQ(pw::bluetooth::emboss::LEOwnAddressType::PUBLIC,
            test_device()->le_scan_state().own_address_type);
}

TEST_F(AdapterTest, LocalAddressForConnections) {
  FakeController::Settings settings;
  settings.ApplyLegacyLEConfig();
  test_device()->set_settings(settings);
  InitializeAdapter([](bool) {});

  // Set-up a device for testing.
  auto* peer =
      adapter()->peer_cache()->NewPeer(kTestAddr, /*connectable=*/true);
  auto fake_peer = std::make_unique<FakePeer>(kTestAddr, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));

  std::unique_ptr<bt::gap::LowEnergyConnectionHandle> conn_ref;
  auto connect_cb = [&conn_ref](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_ref = std::move(result).value();
  };

  // A connection request should use the public address by default.
  adapter()->le()->Connect(
      peer->identifier(), connect_cb, LowEnergyConnectionOptions());

  // Enable privacy. The random address should not get configured while a
  // connection attempt is in progress.
  adapter()->le()->EnablePrivacy(true);
  RunUntilIdle();
  EXPECT_FALSE(test_device()->legacy_advertising_state().random_address);
  ASSERT_TRUE(conn_ref);
  ASSERT_TRUE(test_device()->le_connect_params());
  EXPECT_EQ(pw::bluetooth::emboss::LEOwnAddressType::PUBLIC,
            test_device()->le_connect_params()->own_address_type);

  // Disconnect
  conn_ref = nullptr;

  // Create a new connection. The LowEnergyConnectionManager should try to
  // update the local address before creating the connection.
  adapter()->le()->Connect(
      peer->identifier(), connect_cb, LowEnergyConnectionOptions());
  RunUntilIdle();
  EXPECT_TRUE(test_device()->legacy_advertising_state().random_address);
  ASSERT_TRUE(conn_ref);
  ASSERT_TRUE(test_device()->le_connect_params());

  // TODO: https://fxbug.dev/42141593 - The current policy is to use a public
  // address when initiating connections. Change this test to expect a random
  // address once RPAs for central connections are re-enabled.
  EXPECT_EQ(pw::bluetooth::emboss::LEOwnAddressType::PUBLIC,
            test_device()->le_connect_params()->own_address_type);

  // Disable privacy. The next connection attempt should use a public address.
  adapter()->le()->EnablePrivacy(false);
  conn_ref = nullptr;
  adapter()->le()->Connect(
      peer->identifier(), connect_cb, LowEnergyConnectionOptions());
  RunUntilIdle();
  EXPECT_EQ(pw::bluetooth::emboss::LEOwnAddressType::PUBLIC,
            test_device()->le_connect_params()->own_address_type);
}

// Tests the deferral of random address configuration while a connection request
// is outstanding.
TEST_F(AdapterTest, LocalAddressDuringHangingConnect) {
  FakeController::Settings settings;
  settings.ApplyLegacyLEConfig();
  test_device()->set_settings(settings);
  InitializeAdapter([](bool) {});

  auto* peer =
      adapter()->peer_cache()->NewPeer(kTestAddr, /*connectable=*/true);

  // Cause scanning to succeed and the connection request to hang.
  auto fake_peer = std::make_unique<FakePeer>(kTestAddr, dispatcher());
  fake_peer->set_force_pending_connect(true);
  test_device()->AddPeer(std::move(fake_peer));

  constexpr pw::chrono::SystemClock::duration kTestDelay =
      std::chrono::seconds(5);
  constexpr pw::chrono::SystemClock::duration kTestTimeout =
      kPrivateAddressTimeout + kTestDelay;

  // Some of the behavior below stems from the fact that kTestTimeout is longer
  // than kCacheTimeout. This assertion is here to catch regressions in this
  // test if the values ever change.
  // TODO: https://fxbug.dev/42087236 - Configuring the cache expiration timeout
  // explicitly would remove some of the unnecessary invariants from this test
  // case.
  static_assert(kTestTimeout > kCacheTimeout,
                "expected a shorter device cache timeout");

  adapter()->le()->set_request_timeout_for_testing(kTestTimeout);

  // The connection request should use a public address.
  std::optional<HostError> error;
  int connect_cb_calls = 0;
  auto connect_cb = [&error, &connect_cb_calls](auto result) {
    connect_cb_calls++;
    ASSERT_TRUE(result.is_error());
    error = result.error_value();
  };
  adapter()->le()->Connect(
      peer->identifier(), connect_cb, LowEnergyConnectionOptions());
  RunUntilIdle();
  ASSERT_TRUE(test_device()->le_connect_params());
  EXPECT_EQ(pw::bluetooth::emboss::LEOwnAddressType::PUBLIC,
            test_device()->le_connect_params()->own_address_type);

  // Enable privacy. The random address should not get configured while a
  // connection request is outstanding.
  adapter()->le()->EnablePrivacy(true);
  RunUntilIdle();
  EXPECT_FALSE(test_device()->legacy_advertising_state().random_address);

  // Let the connection request timeout.
  RunFor(kTestTimeout);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(HostError::kTimedOut, error.value())
      << "Error: " << HostErrorToString(error.value());
  EXPECT_EQ(1, connect_cb_calls);

  // The peer should not have expired.
  ASSERT_EQ(peer, adapter()->peer_cache()->FindByAddress(kTestAddr));
  adapter()->le()->Connect(
      peer->identifier(), connect_cb, LowEnergyConnectionOptions());
  RunUntilIdle();
  ASSERT_TRUE(test_device()->legacy_advertising_state().random_address);
  // TODO: https://fxbug.dev/42141593 - The current policy is to use a public
  // address when initiating connections. Change this test to expect a random
  // address once RPAs for central connections are re-enabled.
  EXPECT_EQ(pw::bluetooth::emboss::LEOwnAddressType::PUBLIC,
            test_device()->le_connect_params()->own_address_type);

  // Advance the time to cause the random address to refresh. The update should
  // be deferred while a connection request is outstanding.
  auto last_random_addr =
      *test_device()->legacy_advertising_state().random_address;
  RunFor(kPrivateAddressTimeout);
  EXPECT_EQ(last_random_addr,
            *test_device()->legacy_advertising_state().random_address);

  ASSERT_EQ(peer, adapter()->peer_cache()->FindByAddress(kTestAddr));

  // The address should refresh after the pending request expires and before the
  // next connection attempt.
  RunFor(kTestDelay);
  ASSERT_EQ(2, connect_cb_calls);

  // This will be notified when LowEnergyConnectionManager is destroyed.
  auto noop_connect_cb = [](auto) {};
  adapter()->le()->Connect(peer->identifier(),
                           std::move(noop_connect_cb),
                           LowEnergyConnectionOptions());
  RunUntilIdle();
  EXPECT_NE(last_random_addr,
            *test_device()->legacy_advertising_state().random_address);
  // TODO: https://fxbug.dev/42141593 - The current policy is to use a public
  // address when initiating connections. Change this test to expect a random
  // address once RPAs for central connections are re-enabled.
  EXPECT_EQ(pw::bluetooth::emboss::LEOwnAddressType::PUBLIC,
            test_device()->le_connect_params()->own_address_type);
}

// Tests that existing connections don't prevent an address change.
TEST_F(AdapterTest, ExistingConnectionDoesNotPreventLocalAddressChange) {
  FakeController::Settings settings;
  settings.ApplyLegacyLEConfig();
  test_device()->set_settings(settings);
  InitializeAdapter([](bool) {});

  adapter()->le()->EnablePrivacy(true);

  std::unique_ptr<bt::gap::LowEnergyConnectionHandle> conn_ref;
  auto connect_cb = [&](auto result) {
    ASSERT_EQ(fit::ok(), result);
    conn_ref = std::move(result).value();
    ASSERT_TRUE(conn_ref);
  };

  auto* peer =
      adapter()->peer_cache()->NewPeer(kTestAddr, /*connectable=*/true);
  auto fake_peer = std::make_unique<FakePeer>(kTestAddr, dispatcher());
  test_device()->AddPeer(std::move(fake_peer));
  adapter()->le()->Connect(
      peer->identifier(), connect_cb, LowEnergyConnectionOptions());
  RunUntilIdle();
  // TODO: https://fxbug.dev/42141593 - The current policy is to use a public
  // address when initiating connections. Change this test to expect a random
  // address once RPAs for central connections are re-enabled.
  EXPECT_EQ(pw::bluetooth::emboss::LEOwnAddressType::PUBLIC,
            test_device()->le_connect_params()->own_address_type);

  // Expire the private address. The address should refresh without interference
  // from the ongoing connection.
  ASSERT_TRUE(test_device()->legacy_advertising_state().random_address);
  auto last_random_addr =
      *test_device()->legacy_advertising_state().random_address;
  RunFor(kPrivateAddressTimeout);
  ASSERT_TRUE(test_device()->legacy_advertising_state().random_address);
  EXPECT_NE(last_random_addr,
            *test_device()->legacy_advertising_state().random_address);
}

TEST_F(AdapterTest, IsDiscoverableLowEnergy) {
  FakeController::Settings settings;
  settings.ApplyLegacyLEConfig();
  test_device()->set_settings(settings);
  InitializeAdapter([](bool) {});

  EXPECT_FALSE(adapter()->IsDiscoverable());

  AdvertisementInstance instance;
  adapter()->le()->StartAdvertising(AdvertisingData(),
                                    AdvertisingData(),
                                    AdvertisingInterval::FAST1,
                                    /*extended_pdu=*/false,
                                    /*anonymous=*/false,
                                    /*include_tx_power_level=*/false,
                                    /*connectable=*/std::nullopt,
                                    /*address_type=*/std::nullopt,
                                    [&](AdvertisementInstance i, auto status) {
                                      ASSERT_EQ(fit::ok(), status);
                                      instance = std::move(i);
                                    });
  RunUntilIdle();
  EXPECT_TRUE(adapter()->IsDiscoverable());

  instance = {};
  RunUntilIdle();
  EXPECT_FALSE(adapter()->IsDiscoverable());
}

TEST_F(AdapterTest, IsDiscoverableBredr) {
  FakeController::Settings settings;
  settings.ApplyDualModeDefaults();
  test_device()->set_settings(settings);
  InitializeAdapter([](bool) {});

  EXPECT_FALSE(adapter()->IsDiscoverable());

  std::unique_ptr<BrEdrDiscoverableSession> session;
  adapter()->bredr()->RequestDiscoverable(
      [&](auto, auto s) { session = std::move(s); });
  RunUntilIdle();
  EXPECT_TRUE(adapter()->IsDiscoverable());

  session = nullptr;
  RunUntilIdle();
  EXPECT_FALSE(adapter()->IsDiscoverable());
}

TEST_F(AdapterTest, IsDiscoverableLowEnergyPrivacyEnabled) {
  FakeController::Settings settings;
  settings.ApplyLegacyLEConfig();
  test_device()->set_settings(settings);
  InitializeAdapter([](bool) {});

  EXPECT_FALSE(adapter()->IsDiscoverable());
  adapter()->le()->EnablePrivacy(true);
  EXPECT_FALSE(adapter()->IsDiscoverable());

  AdvertisementInstance instance;
  adapter()->le()->StartAdvertising(AdvertisingData(),
                                    AdvertisingData(),
                                    AdvertisingInterval::FAST1,
                                    /*extended_pdu=*/false,
                                    /*anonymous=*/false,
                                    /*include_tx_power_level=*/false,
                                    /*connectable=*/std::nullopt,
                                    /*address_type=*/std::nullopt,
                                    [&](AdvertisementInstance i, auto status) {
                                      ASSERT_EQ(fit::ok(), status);
                                      instance = std::move(i);
                                    });
  RunUntilIdle();
  // Even though we are advertising over LE, we are not discoverable since
  // Privacy is enabled.
  EXPECT_FALSE(adapter()->IsDiscoverable());

  instance = {};
  RunUntilIdle();
  EXPECT_FALSE(adapter()->IsDiscoverable());
}

#ifndef NINSPECT
TEST_F(AdapterTest, InspectHierarchy) {
  inspect::Inspector inspector;
  auto bt_host_node = inspector.GetRoot().CreateChild("bt-host");
  adapter()->AttachInspect(bt_host_node, "adapter");

  bool success;
  int init_cb_count = 0;
  auto init_cb = [&](bool cb_success) {
    success = cb_success;
    init_cb_count++;
  };

  // Return valid buffer information and enable LE support. (This should
  // succeed).
  FakeController::Settings settings;
  settings.AddBREDRSupportedCommands();
  settings.lmp_features_page0 |=
      static_cast<uint64_t>(hci_spec::LMPFeature::kLESupportedHost);
  settings.le_acl_data_packet_length = 0x1B;
  settings.le_total_num_acl_data_packets = 1;
  settings.synchronous_data_packet_length = 6;
  settings.total_num_synchronous_data_packets = 2;
  test_device()->set_settings(settings);

  InitializeAdapter(std::move(init_cb));
  EXPECT_TRUE(success);

  auto le_connection_manager_matcher =
      NodeMatches(NameMatches("low_energy_connection_manager"));
  auto bredr_connection_manager_matcher =
      NodeMatches(NameMatches("bredr_connection_manager"));
  auto peer_cache_matcher =
      NodeMatches(NameMatches(PeerCache::kInspectNodeName));
  auto sdp_server_matcher =
      NodeMatches(NameMatches(sdp::Server::kInspectNodeName));
  auto acl_data_channel_matcher =
      NodeMatches(NameMatches(hci::AclDataChannel::kInspectNodeName));
  auto le_matcher =
      AllOf(NodeMatches(AllOf(NameMatches("le"),
                              PropertyList(UnorderedElementsAre(
                                  UintIs("open_l2cap_channel_requests", 0),
                                  UintIs("outgoing_connection_requests", 0),
                                  UintIs("pair_requests", 0),
                                  UintIs("start_advertising_events", 0),
                                  UintIs("stop_advertising_events", 0),
                                  UintIs("start_discovery_events", 0))))));
  auto bredr_matcher =
      AllOf(NodeMatches(AllOf(NameMatches("bredr"),
                              PropertyList(UnorderedElementsAre(
                                  UintIs("outgoing_connection_requests", 0),
                                  UintIs("pair_requests", 0),
                                  UintIs("set_connectable_true_events", 0),
                                  UintIs("set_connectable_false_events", 0),
                                  UintIs("open_l2cap_channel_requests", 0))))));
  auto metrics_node_matcher =
      AllOf(NodeMatches(NameMatches(Adapter::kMetricsInspectNodeName)),
            ChildrenMatch(UnorderedElementsAre(bredr_matcher, le_matcher)));
  auto le_discovery_manager_matcher =
      NodeMatches(NameMatches("low_energy_discovery_manager"));
  auto bredr_discovery_manager_matcher =
      NodeMatches(NameMatches("bredr_discovery_manager"));
  auto hci_matcher = NodeMatches(NameMatches(hci::Transport::kInspectNodeName));

  auto adapter_matcher = AllOf(
      NodeMatches(AllOf(
          NameMatches("adapter"),
          PropertyList(UnorderedElementsAre(
              StringIs("adapter_id", adapter()->identifier().ToString()),
              StringIs(
                  "hci_version",
                  hci_spec::HCIVersionToString(adapter()->state().hci_version)),
              UintIs(
                  "bredr_max_num_packets",
                  adapter()->state().bredr_data_buffer_info.max_num_packets()),
              UintIs(
                  "bredr_max_data_length",
                  adapter()->state().bredr_data_buffer_info.max_data_length()),
              UintIs("le_max_num_packets",
                     adapter()
                         ->state()
                         .low_energy_state.acl_data_buffer_info()
                         .max_num_packets()),
              UintIs("le_max_data_length",
                     adapter()
                         ->state()
                         .low_energy_state.acl_data_buffer_info()
                         .max_data_length()),
              UintIs("sco_max_num_packets",
                     adapter()->state().sco_buffer_info.max_num_packets()),
              UintIs("sco_max_data_length",
                     adapter()->state().sco_buffer_info.max_data_length()),
              StringIs("lmp_features", adapter()->state().features.ToString()),
              StringIs("le_features",
                       bt_lib_cpp_string::StringPrintf(
                           "0x%016lx",
                           adapter()
                               ->state()
                               .low_energy_state.supported_features())))))),
      ChildrenMatch(UnorderedElementsAre(peer_cache_matcher,
                                         sdp_server_matcher,
                                         le_connection_manager_matcher,
                                         bredr_connection_manager_matcher,
                                         le_discovery_manager_matcher,
                                         metrics_node_matcher,
                                         bredr_discovery_manager_matcher,
                                         hci_matcher)));

  auto bt_host_matcher =
      AllOf(NodeMatches(NameMatches("bt-host")),
            ChildrenMatch(UnorderedElementsAre(adapter_matcher)));
  auto hierarchy = inspect::ReadFromVmo(inspector.DuplicateVmo()).take_value();

  EXPECT_THAT(hierarchy,
              AllOf(NodeMatches(NameMatches("root")),
                    ChildrenMatch(UnorderedElementsAre(bt_host_matcher))));
}
#endif  // NINSPECT

TEST_F(AdapterTest, VendorFeatures) {
  FakeController::Settings settings;
  settings.ApplyDualModeDefaults();
  test_device()->set_settings(settings);

  bool success = false;
  auto init_cb = [&](bool cb_success) { success = cb_success; };
  InitializeAdapter(std::move(init_cb));
  EXPECT_TRUE(success);
  EXPECT_EQ(adapter()->state().controller_features, kDefaultFeaturesBits);
}

TEST_F(AdapterTest,
       LowEnergyStartAdvertisingConnectCallbackReceivesConnection) {
  FakeController::Settings settings;
  settings.ApplyLegacyLEConfig();
  test_device()->set_settings(settings);
  InitializeAdapter([](bool) {});

  AdvertisementInstance instance;
  auto adv_cb = [&](auto i, hci::Result<> status) {
    instance = std::move(i);
    EXPECT_EQ(fit::ok(), status);
  };

  std::optional<Adapter::LowEnergy::ConnectionResult> conn_result;
  std::optional<AdvertisementId> conn_cb_advertisement_id;
  Adapter::LowEnergy::ConnectionCallback connect_cb =
      [&](AdvertisementId adv_id, Adapter::LowEnergy::ConnectionResult result) {
        conn_result = std::move(result);
        conn_cb_advertisement_id = adv_id;
      };

  adapter()->le()->StartAdvertising(
      AdvertisingData(),
      AdvertisingData(),
      AdvertisingInterval::FAST1,
      /*extended_pdu=*/false,
      /*anonymous=*/false,
      /*include_tx_power_level=*/false,
      bt::gap::Adapter::LowEnergy::ConnectableAdvertisingParameters{
          .connection_cb = std::move(connect_cb),
          .bondable_mode = sm::BondableMode::NonBondable},
      /*address_type=*/std::nullopt,
      adv_cb);
  RunUntilIdle();
  EXPECT_FALSE(conn_result);

  fit::closure complete_interrogation;
  // Pause interrogation so we can control when the inbound connection procedure
  // completes.
  test_device()->pause_responses_for_opcode(
      bt::hci_spec::kReadRemoteVersionInfo, [&](fit::closure trigger) {
        complete_interrogation = std::move(trigger);
      });

  test_device()->AddPeer(std::make_unique<FakePeer>(kTestAddr, dispatcher()));
  test_device()->ConnectLowEnergy(kTestAddr);
  RunUntilIdle();
  ASSERT_FALSE(conn_result);
  ASSERT_TRUE(complete_interrogation);

  complete_interrogation();
  RunUntilIdle();
  ASSERT_TRUE(conn_result);
  ASSERT_EQ(fit::ok(), *conn_result);
  std::unique_ptr<LowEnergyConnectionHandle> conn_handle =
      std::move(*conn_result).value();
  ASSERT_TRUE(conn_handle);
  EXPECT_EQ(conn_handle->bondable_mode(), sm::BondableMode::NonBondable);
  EXPECT_EQ(*conn_cb_advertisement_id, instance.id());
  conn_result.reset();
}

TEST_F(AdapterTest, CreateAdvertiserExtendedAdvertisingSupported) {
  FakeController::Settings settings;
  settings.ApplyExtendedLEConfig();
  test_device()->set_settings(settings);
  InitializeAdapter([](bool) {});

  adapter()->le()->StartAdvertising(AdvertisingData(),
                                    AdvertisingData(),
                                    AdvertisingInterval::FAST1,
                                    /*extended_pdu=*/false,
                                    /*anonymous=*/false,
                                    /*include_tx_power_level=*/false,
                                    /*connectable=*/std::nullopt,
                                    /*address_type=*/std::nullopt,
                                    [&](AdvertisementInstance, auto status) {
                                      ASSERT_EQ(fit::ok(), status);
                                    });
  RunUntilIdle();
  EXPECT_EQ(bt::testing::FakeController::ExtendedOperationType::kExtended,
            test_device()->advertising_procedure());
}

TEST_F(AdapterTest,
       CreateAdvertiserVendorAdvertisingSupportedExtendedSupported) {
  TearDown();
  SetUp(FeaturesBits::kAndroidVendorExtensions);

  FakeController::Settings settings;
  settings.ApplyExtendedLEConfig();
  test_device()->set_settings(settings);
  InitializeAdapter([](bool) {});

  adapter()->le()->StartAdvertising(AdvertisingData(),
                                    AdvertisingData(),
                                    AdvertisingInterval::FAST1,
                                    /*extended_pdu=*/false,
                                    /*anonymous=*/false,
                                    /*include_tx_power_level=*/false,
                                    /*connectable=*/std::nullopt,
                                    /*address_type=*/std::nullopt,
                                    [&](AdvertisementInstance, auto status) {
                                      ASSERT_EQ(fit::ok(), status);
                                    });
  RunUntilIdle();
  EXPECT_EQ(bt::testing::FakeController::ExtendedOperationType::kExtended,
            test_device()->advertising_procedure());
}

TEST_F(AdapterTest,
       CreateAdvertiserVendorAdvertisingSupportedExtendedUnsupported) {
  TearDown();
  SetUp(FeaturesBits::kAndroidVendorExtensions);

  FakeController::Settings settings;
  settings.ApplyLegacyLEConfig();
  settings.ApplyAndroidVendorExtensionDefaults();
  test_device()->set_settings(settings);
  InitializeAdapter([](bool) {});

  adapter()->le()->StartAdvertising(AdvertisingData(),
                                    AdvertisingData(),
                                    AdvertisingInterval::FAST1,
                                    /*extended_pdu=*/false,
                                    /*anonymous=*/false,
                                    /*include_tx_power_level=*/false,
                                    /*connectable=*/std::nullopt,
                                    /*address_type=*/std::nullopt,
                                    [&](AdvertisementInstance, auto status) {
                                      ASSERT_EQ(fit::ok(), status);
                                    });
  RunUntilIdle();
  EXPECT_EQ(bt::testing::FakeController::ExtendedOperationType::kVendor,
            test_device()->advertising_procedure());
}

TEST_F(AdapterTest, CreateAdvertiserLegacyAdvertisingSupported) {
  FakeController::Settings settings;
  settings.ApplyLegacyLEConfig();
  test_device()->set_settings(settings);
  InitializeAdapter([](bool) {});

  adapter()->le()->StartAdvertising(AdvertisingData(),
                                    AdvertisingData(),
                                    AdvertisingInterval::FAST1,
                                    /*extended_pdu=*/false,
                                    /*anonymous=*/false,
                                    /*include_tx_power_level=*/false,
                                    /*connectable=*/std::nullopt,
                                    /*address_type=*/std::nullopt,
                                    [&](AdvertisementInstance, auto status) {
                                      ASSERT_EQ(fit::ok(), status);
                                    });
  RunUntilIdle();
  EXPECT_EQ(bt::testing::FakeController::ExtendedOperationType::kLegacy,
            test_device()->advertising_procedure());
}

// Tests where the constructor must run in the test, rather than Setup.

class AdapterConstructorTest : public TestingBase {
 public:
  AdapterConstructorTest() = default;
  ~AdapterConstructorTest() override = default;

  void SetUp() override {
    TestingBase::SetUp();

    l2cap_ = std::make_unique<l2cap::testing::FakeL2cap>(dispatcher());
    gatt_ = std::make_unique<gatt::testing::FakeLayer>(dispatcher());
  }

  void TearDown() override {
    l2cap_ = nullptr;
    gatt_ = nullptr;
    TestingBase::TearDown();
  }

  pw::bluetooth_sapphire::testing::FakeLeaseProvider& lease_provider() {
    return lease_provider_;
  }

 protected:
  pw::bluetooth_sapphire::testing::FakeLeaseProvider lease_provider_;
  std::unique_ptr<l2cap::testing::FakeL2cap> l2cap_;
  std::unique_ptr<gatt::testing::FakeLayer> gatt_;
};

using GAP_AdapterConstructorTest = AdapterConstructorTest;

TEST_F(AdapterConstructorTest, GattCallbacks) {
  constexpr PeerId kPeerId(1234);
  constexpr gatt::ServiceChangedCCCPersistedData kPersistedData = {
      .notify = true, .indicate = true};

  int set_persist_cb_count = 0;
  int set_retrieve_cb_count = 0;

  auto set_persist_cb_cb = [&set_persist_cb_count]() {
    set_persist_cb_count++;
  };

  auto set_retrieve_cb_cb = [&set_retrieve_cb_count]() {
    set_retrieve_cb_count++;
  };

  gatt_->SetSetPersistServiceChangedCCCCallbackCallback(set_persist_cb_cb);
  gatt_->SetSetRetrieveServiceChangedCCCCallbackCallback(set_retrieve_cb_cb);

  EXPECT_EQ(set_persist_cb_count, 0);
  EXPECT_EQ(set_retrieve_cb_count, 0);

  Adapter::Config config = {
      .legacy_pairing_enabled = false,
  };
  auto adapter = Adapter::Create(dispatcher(),
                                 transport()->GetWeakPtr(),
                                 gatt_->GetWeakPtr(),
                                 config,
                                 lease_provider(),
                                 std::move(l2cap_));

  EXPECT_EQ(set_persist_cb_count, 1);
  EXPECT_EQ(set_retrieve_cb_count, 1);

  // Before the peer exists, adding its gatt info to the peer cache does
  // nothing.
  gatt_->CallPersistServiceChangedCCCCallback(
      kPeerId, /*notify=*/true, /*indicate=*/false);
  auto persisted_data_1 = gatt_->CallRetrieveServiceChangedCCCCallback(kPeerId);
  EXPECT_EQ(persisted_data_1, std::nullopt);

  // After adding a classic peer, adding its info to the peer cache still does
  // nothing.
  Peer* classic_peer =
      adapter->peer_cache()->NewPeer(kTestAddrBrEdr, /*connectable=*/true);
  PeerId classic_peer_id = classic_peer->identifier();

  gatt_->CallPersistServiceChangedCCCCallback(
      classic_peer_id, /*notify=*/false, /*indicate=*/true);
  auto persisted_data_2 =
      gatt_->CallRetrieveServiceChangedCCCCallback(classic_peer_id);
  EXPECT_EQ(persisted_data_2, std::nullopt);

  // After adding an LE peer, adding its info to the peer cache works.
  Peer* le_peer =
      adapter->peer_cache()->NewPeer(kTestAddr, /*connectable=*/true);
  PeerId le_peer_id = le_peer->identifier();

  gatt_->CallPersistServiceChangedCCCCallback(
      le_peer_id, /*notify=*/true, /*indicate=*/true);
  auto persisted_data_3 =
      gatt_->CallRetrieveServiceChangedCCCCallback(le_peer_id);
  ASSERT_TRUE(persisted_data_3.has_value());
  auto persisted_data_3_value = persisted_data_3.value();
  EXPECT_EQ(persisted_data_3_value, kPersistedData);

  // After the peer is removed, the gatt info is no longer in the peer cache.
  bool result = adapter->peer_cache()->RemoveDisconnectedPeer(le_peer_id);
  EXPECT_TRUE(result);

  auto persisted_data_4 =
      gatt_->CallRetrieveServiceChangedCCCCallback(le_peer_id);
  EXPECT_EQ(persisted_data_4, std::nullopt);
}

TEST_F(AdapterTest, BufferSizesRecordedInState) {
  bool success = false;
  auto init_cb = [&](bool cb_success) { success = cb_success; };

  FakeController::Settings settings;
  // Enable ReadBuffer commands.
  settings.AddBREDRSupportedCommands();
  settings.AddLESupportedCommands();
  settings.lmp_features_page0 |=
      static_cast<uint64_t>(hci_spec::LMPFeature::kLESupportedHost);

  // Minimum supported size as per Core Spec v5.4, Vol 4, Part E, 7.8.2
  settings.le_acl_data_packet_length = 0x1B;

  settings.le_total_num_acl_data_packets = 2;
  settings.acl_data_packet_length = 3;
  settings.total_num_acl_data_packets = 4;
  settings.synchronous_data_packet_length = 5;
  settings.total_num_synchronous_data_packets = 6;
  settings.iso_data_packet_length = 7;
  settings.total_num_iso_data_packets = 8;
  test_device()->set_settings(settings);

  InitializeAdapter(std::move(init_cb));
  EXPECT_TRUE(success);
  EXPECT_EQ(adapter()
                ->state()
                .low_energy_state.acl_data_buffer_info()
                .max_data_length(),
            (uint16_t)0x1B);
  EXPECT_EQ(adapter()
                ->state()
                .low_energy_state.acl_data_buffer_info()
                .max_num_packets(),
            2u);
  EXPECT_EQ(adapter()->state().bredr_data_buffer_info.max_data_length(), 3u);
  EXPECT_EQ(adapter()->state().bredr_data_buffer_info.max_num_packets(), 4u);
  EXPECT_EQ(adapter()->state().sco_buffer_info.max_data_length(), 5u);
  EXPECT_EQ(adapter()->state().sco_buffer_info.max_num_packets(), 6u);
  EXPECT_EQ(adapter()
                ->state()
                .low_energy_state.iso_data_buffer_info()
                .max_data_length(),
            7u);
  EXPECT_EQ(adapter()
                ->state()
                .low_energy_state.iso_data_buffer_info()
                .max_num_packets(),
            8u);
}

TEST_F(AdapterTest, LEReadMaximumAdvertisingDataLengthNotSupported) {
  FakeController::Settings settings;
  settings.AddBREDRSupportedCommands();
  settings.AddLESupportedCommands();
  settings.lmp_features_page0 |=
      static_cast<uint64_t>(hci_spec::LMPFeature::kLESupportedHost);
  settings.le_acl_data_packet_length = 0x1B;
  settings.le_total_num_acl_data_packets = 2;
  test_device()->set_settings(settings);

  const LowEnergyState& low_energy_state = adapter()->state().low_energy_state;
  bool success = false;
  auto init_cb = [&](bool cb_success) { success = cb_success; };
  InitializeAdapter(std::move(init_cb));
  EXPECT_TRUE(success);
  EXPECT_EQ(hci_spec::kMaxLEAdvertisingDataLength,
            low_energy_state.max_advertising_data_length());
}

TEST_F(AdapterTest, LEReadMaximumAdvertisingDataLengthSupported) {
  FakeController::Settings settings;
  settings.AddBREDRSupportedCommands();
  settings.AddLESupportedCommands();
  settings.lmp_features_page0 |=
      static_cast<uint64_t>(hci_spec::LMPFeature::kLESupportedHost);
  settings.le_acl_data_packet_length = 0x1B;
  settings.le_total_num_acl_data_packets = 2;
  settings.SupportedCommandsView()
      .le_read_maximum_advertising_data_length()
      .Write(true);

  test_device()->set_settings(settings);
  test_device()->set_maximum_advertising_data_length(
      hci_spec::kMaxLEExtendedAdvertisingDataLength);

  const LowEnergyState& low_energy_state = adapter()->state().low_energy_state;
  bool success = false;
  auto init_cb = [&](bool cb_success) { success = cb_success; };
  InitializeAdapter(std::move(init_cb));
  EXPECT_TRUE(success);
  EXPECT_EQ(hci_spec::kMaxLEExtendedAdvertisingDataLength,
            low_energy_state.max_advertising_data_length());
}

TEST_F(AdapterTest, LEConnectedIsochronousStreamSupported) {
  FakeController::Settings settings;
  settings.AddBREDRSupportedCommands();
  settings.AddLESupportedCommands();
  settings.lmp_features_page0 |=
      static_cast<uint64_t>(hci_spec::LMPFeature::kLESupportedHost);
  settings.le_features |= static_cast<uint64_t>(
      hci_spec::LESupportedFeature::kConnectedIsochronousStreamPeripheral);
  settings.le_acl_data_packet_length = 0x1B;
  settings.le_total_num_acl_data_packets = 2;
  settings.iso_data_packet_length = 0xc0;
  settings.total_num_iso_data_packets = 8;

  test_device()->set_settings(settings);

  bool success = false;
  auto init_cb = [&](bool cb_success) { success = cb_success; };
  InitializeAdapter(std::move(init_cb));
  EXPECT_TRUE(success);
  const auto& le_features = test_device()->le_features();
  EXPECT_TRUE(
      (le_features &
       static_cast<uint64_t>(hci_spec::LESupportedFeature::
                                 kConnectedIsochronousStreamHostSupport)) != 0);
}

TEST_F(AdapterTest, ScoDataChannelInitializedSuccessfully) {
  // Return valid buffer information and enable LE support.
  FakeController::Settings settings;
  settings.AddBREDRSupportedCommands();
  settings.lmp_features_page0 |=
      static_cast<uint64_t>(hci_spec::LMPFeature::kLESupportedHost);
  settings.le_acl_data_packet_length = 0x1B;
  settings.le_total_num_acl_data_packets = 1;
  // Ensure SCO buffers are available.
  settings.synchronous_data_packet_length = 6;
  settings.total_num_synchronous_data_packets = 2;
  // Enable SCO flow control command.
  settings.SupportedCommandsView()
      .write_synchronous_flow_control_enable()
      .Write(true);
  test_device()->set_settings(settings);

  bool success = false;
  auto init_cb = [&](bool cb_success) { success = cb_success; };
  InitializeAdapter(std::move(init_cb));
  EXPECT_TRUE(success);
  EXPECT_TRUE(transport()->sco_data_channel());
}

TEST_F(AdapterTest,
       ScoDataChannelNotInitializedBecauseFlowControlNotSupported) {
  // Return valid buffer information and enable LE support.
  FakeController::Settings settings;
  settings.AddBREDRSupportedCommands();
  settings.lmp_features_page0 |=
      static_cast<uint64_t>(hci_spec::LMPFeature::kLESupportedHost);
  settings.SupportedCommandsView()
      .write_synchronous_flow_control_enable()
      .Write(false);
  settings.le_acl_data_packet_length = 0x1B;
  settings.le_total_num_acl_data_packets = 1;
  // Ensure SCO buffers are available.
  settings.synchronous_data_packet_length = 6;
  settings.total_num_synchronous_data_packets = 2;
  test_device()->set_settings(settings);

  bool success = false;
  auto init_cb = [&](bool cb_success) { success = cb_success; };
  InitializeAdapter(std::move(init_cb));
  EXPECT_TRUE(success);
  EXPECT_FALSE(transport()->sco_data_channel());
}

TEST_F(AdapterTest, ScoDataChannelNotInitializedBecauseBufferInfoNotAvailable) {
  // Return valid buffer information and enable LE support.
  FakeController::Settings settings;
  settings.AddBREDRSupportedCommands();
  settings.lmp_features_page0 |=
      static_cast<uint64_t>(hci_spec::LMPFeature::kLESupportedHost);
  settings.le_acl_data_packet_length = 0x1B;
  settings.le_total_num_acl_data_packets = 1;
  // Ensure SCO buffers are not available.
  settings.synchronous_data_packet_length = 1;
  settings.total_num_synchronous_data_packets = 0;
  // Enable SCO flow control command.
  settings.SupportedCommandsView()
      .write_synchronous_flow_control_enable()
      .Write(true);
  test_device()->set_settings(settings);

  bool success = false;
  auto init_cb = [&](bool cb_success) { success = cb_success; };
  InitializeAdapter(std::move(init_cb));
  EXPECT_TRUE(success);
  EXPECT_FALSE(transport()->sco_data_channel());
}

TEST_F(AdapterScoAndIsoDisabledTest,
       ScoDataChannelFailsToInitializeBecauseScoDisabled) {
  // Return valid buffer information and enable LE support.
  FakeController::Settings settings;
  settings.AddBREDRSupportedCommands();
  settings.lmp_features_page0 |=
      static_cast<uint64_t>(hci_spec::LMPFeature::kLESupportedHost);
  settings.le_acl_data_packet_length = 0x1B;
  settings.le_total_num_acl_data_packets = 1;
  // Ensure SCO buffers are available.
  settings.synchronous_data_packet_length = 6;
  settings.total_num_synchronous_data_packets = 2;
  // Enable SCO flow control command.
  settings.SupportedCommandsView()
      .write_synchronous_flow_control_enable()
      .Write(true);
  test_device()->set_settings(settings);

  bool success = false;
  auto init_cb = [&](bool cb_success) { success = cb_success; };
  InitializeAdapter(std::move(init_cb));
  EXPECT_TRUE(success);
  EXPECT_FALSE(transport()->sco_data_channel());
}

TEST_F(AdapterTest, IsoDataChannelInitializedSuccessfully) {
  FakeController::Settings settings;
  settings.ApplyDualModeDefaults();
  settings.le_features |= static_cast<uint64_t>(
      hci_spec::LESupportedFeature::kConnectedIsochronousStreamPeripheral);
  test_device()->set_settings(settings);

  bool success = false;
  auto init_cb = [&](bool cb_success) { success = cb_success; };
  InitializeAdapter(std::move(init_cb));
  EXPECT_TRUE(success);
  EXPECT_TRUE(transport()->iso_data_channel());
}

TEST_F(AdapterTest, IsoDataChannelNotInitializedNoBufferData) {
  FakeController::Settings settings;
  settings.ApplyDualModeDefaults();
  settings.iso_data_packet_length = 0;
  settings.total_num_iso_data_packets = 0;
  test_device()->set_settings(settings);

  bool success = false;
  auto init_cb = [&](bool cb_success) { success = cb_success; };
  InitializeAdapter(std::move(init_cb));
  EXPECT_TRUE(success);
  EXPECT_FALSE(transport()->iso_data_channel());
}

TEST_F(AdapterScoAndIsoDisabledTest, IsoDataChannelNoControllerSupport) {
  FakeController::Settings settings;
  settings.ApplyDualModeDefaults();
  test_device()->set_settings(settings);

  bool success = false;
  auto init_cb = [&](bool cb_success) { success = cb_success; };
  InitializeAdapter(std::move(init_cb));
  EXPECT_TRUE(success);
  EXPECT_FALSE(transport()->iso_data_channel());
}

TEST_F(AdapterTest, InitializeWriteSecureConnectionsHostSupport) {
  bool success;
  int init_cb_count = 0;
  auto init_cb = [&](bool cb_success) {
    success = cb_success;
    init_cb_count++;
  };

  // Enable LE support and extended features (Secure Connections Host Support)
  FakeController::Settings settings;
  settings.lmp_features_page0 |=
      static_cast<uint64_t>(hci_spec::LMPFeature::kLESupportedHost);
  settings.lmp_features_page0 |=
      static_cast<uint64_t>(hci_spec::LMPFeature::kExtendedFeatures);
  settings.lmp_features_page1 |= static_cast<uint64_t>(
      hci_spec::LMPFeature::kSecureConnectionsHostSupport);
  settings.le_acl_data_packet_length = 0x1B;
  settings.le_total_num_acl_data_packets = 1;
  test_device()->set_settings(settings);

  InitializeAdapter(std::move(init_cb));
  EXPECT_TRUE(success);
  EXPECT_EQ(1, init_cb_count);
  EXPECT_TRUE(adapter()->state().IsLowEnergySupported());
  EXPECT_TRUE(adapter()->state().IsBREDRSupported());
  EXPECT_TRUE(adapter()->state().IsSecureConnectionHostSupportSupported());
  EXPECT_TRUE(adapter()->le());
  EXPECT_TRUE(adapter()->bredr());
  EXPECT_EQ(TechnologyType::kDualMode, adapter()->state().type());
  EXPECT_FALSE(transport_closed_called());
}

void AdapterTest::GetSupportedDelayRangeHelper(
    bool supported, const std::optional<std::vector<uint8_t>>& codec_config) {
  FakeController::Settings settings;

  // Define minimum required settings for an LE controller
  settings.lmp_features_page0 |=
      static_cast<uint64_t>(hci_spec::LMPFeature::kLESupportedHost);
  settings.le_acl_data_packet_length = 0x1B;
  settings.le_total_num_acl_data_packets = 1;

  // Enable or disable the "Read Local Supported Controller Delay" command
  settings.SupportedCommandsView()
      .read_local_supported_controller_delay()
      .Write(supported);

  test_device()->set_settings(settings);

  bool init_success = false;
  InitializeAdapter([&](bool success) { init_success = success; });
  ASSERT_TRUE(init_success);

  // Error code that should never be returned
  pw::Status cb_status = static_cast<pw::Status::Code>(PW_STATUS_LAST + 1);
  uint32_t min_delay_us = -1;
  uint32_t max_delay_us = -1;
  Adapter::GetSupportedDelayRangeCallback cb =
      [&](pw::Status status, uint32_t min_delay, uint32_t max_delay) {
        cb_status = status;
        min_delay_us = min_delay;
        max_delay_us = max_delay;
      };

  // Construct codec ID information
  StaticPacket<pw::bluetooth::emboss::CodecIdWriter> codec_id;
  codec_id.view().coding_format().Write(
      pw::bluetooth::emboss::CodingFormat::U_LAW);
  codec_id.view().company_id().Write(0u);
  codec_id.view().vendor_codec_id().Write(0u);

  adapter()->GetSupportedDelayRange(
      codec_id,
      pw::bluetooth::emboss::LogicalTransportType::BR_EDR_ACL,
      pw::bluetooth::emboss::DataPathDirection::INPUT,
      codec_config,
      std::move(cb));
  RunUntilIdle();
  if (!supported) {
    EXPECT_EQ(cb_status, PW_STATUS_UNIMPLEMENTED);
  } else {
    EXPECT_TRUE(cb_status.ok());
    EXPECT_LE(min_delay_us, max_delay_us);
    EXPECT_LE(max_delay_us, 0x3D0900u);
  }
}

// Verify proper behavior when "Read Local Supported Controller Delay" command
// is not supported in the controller.
TEST_F(AdapterTest, ReadLocalSupportedControllerDelayNotSupported) {
  GetSupportedDelayRangeHelper(false, std::nullopt);
}

// Verify proper behavior when "Read Local Supported Controller Delay" command
// is supported in the controller and behaves properly when codec configuration
// is empty.
TEST_F(AdapterTest, ReadLocalSupportedControllerDelayBasic) {
  GetSupportedDelayRangeHelper(true, std::nullopt);
}
// Verify proper behavior when "Read Local Supported Controller Delay" command
// is supported in the controller and behaves properly when codec configuration
// is present.
TEST_F(AdapterTest, ReadLocalSupportedControllerDelayWithCodecConfig) {
  std::vector<uint8_t> codec_configuration{0x11, 0x22, 0x33, 0x44, 0x55};
  GetSupportedDelayRangeHelper(true, codec_configuration);
}

TEST_F(AdapterTest, RemotePublicKeyValidationSupported) {
  FakeController::Settings settings;
  settings.ApplyDualModeDefaults();
  settings.SupportedCommandsView().read_local_simple_pairing_options().Write(
      true);
  test_device()->set_settings(settings);
  EXPECT_TRUE(EnsureInitialized());
  EXPECT_TRUE(
      adapter()->state().IsControllerRemotePublicKeyValidationSupported());
}

TEST_F(AdapterTest, RemotePublicKeyValidationNotSupported) {
  FakeController::Settings settings;
  settings.ApplyDualModeDefaults();
  settings.SupportedCommandsView().read_local_simple_pairing_options().Write(
      false);
  test_device()->set_settings(settings);
  EXPECT_TRUE(EnsureInitialized());
  EXPECT_FALSE(
      adapter()->state().IsControllerRemotePublicKeyValidationSupported());
}

struct PeriodicAdvertisingSyncDelegate
    : public Adapter::LowEnergy::PeriodicAdvertisingSyncDelegate {
 public:
  void OnSyncEstablished(
      hci::SyncId, PeriodicAdvertisingSyncManager::SyncParameters) override {}
  void OnSyncLost(hci::SyncId, hci::Error) override {}
  void OnAdvertisingReport(hci::SyncId,
                           const PeriodicAdvertisingReport&) override {}
  void OnBigInfoReport(
      hci::SyncId, const hci_spec::BroadcastIsochronousGroupInfo&) override {}
};

TEST_F(AdapterTest, PeriodicAdvertisingSynchronizationSupported) {
  FakeController::Settings settings;
  settings.ApplyDualModeDefaults();
  settings.le_features |= static_cast<uint64_t>(
      hci_spec::LESupportedFeature::kSynchronizedReceiver);
  test_device()->set_settings(settings);
  EXPECT_TRUE(EnsureInitialized());
  EXPECT_TRUE(adapter()->state().low_energy_state.IsFeatureSupported(
      hci_spec::LESupportedFeature::kSynchronizedReceiver));

  Peer* le_peer =
      adapter()->peer_cache()->NewPeer(kTestAddr, /*connectable=*/true);
  uint8_t adv_sid = 8;
  Adapter::LowEnergy::SyncOptions options{.filter_duplicates = true};
  PeriodicAdvertisingSyncDelegate delegate;

  hci::Result<PeriodicAdvertisingSyncHandle> result =
      adapter()->le()->SyncToPeriodicAdvertisement(
          le_peer->identifier(), adv_sid, options, delegate);
  EXPECT_TRUE(result.is_ok());
}

TEST_F(AdapterTest, PeriodicAdvertisingSynchronizationNotSupported) {
  FakeController::Settings settings;
  settings.ApplyDualModeDefaults();
  settings.le_features &= ~static_cast<uint64_t>(
      hci_spec::LESupportedFeature::kSynchronizedReceiver);
  test_device()->set_settings(settings);
  EXPECT_TRUE(EnsureInitialized());
  EXPECT_FALSE(adapter()->state().low_energy_state.IsFeatureSupported(
      hci_spec::LESupportedFeature::kSynchronizedReceiver));

  Peer* le_peer =
      adapter()->peer_cache()->NewPeer(kTestAddr, /*connectable=*/true);
  uint8_t adv_sid = 8;
  Adapter::LowEnergy::SyncOptions options{.filter_duplicates = true};
  PeriodicAdvertisingSyncDelegate delegate;

  hci::Result<PeriodicAdvertisingSyncHandle> result =
      adapter()->le()->SyncToPeriodicAdvertisement(
          le_peer->identifier(), adv_sid, options, delegate);
  ASSERT_TRUE(result.is_error());
  EXPECT_EQ(result.error_value(), Error(HostError::kNotSupported));
}

}  // namespace
}  // namespace bt::gap
