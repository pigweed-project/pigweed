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

#include "pw_bluetooth_sapphire/internal/host/gap/peer.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <pw_assert/check.h>
#include <pw_async/fake_dispatcher_fixture.h>

#include "pw_bluetooth_sapphire/internal/host/common/advertising_data.h"
#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/common/manufacturer_names.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/util.h"
#include "pw_bluetooth_sapphire/internal/host/testing/inspect_util.h"
#include "pw_bluetooth_sapphire/internal/host/transport/emboss_packet.h"

namespace bt::gap {
namespace {

#ifndef NINSPECT
using namespace inspect::testing;
using bt::testing::GetInspectValue;
using bt::testing::ReadInspect;

constexpr uint16_t kManufacturer = 0x0001;
constexpr uint16_t kSubversion = 0x0002;
#endif  // NINSPECT

const StaticByteBuffer kAdvData(0x05,  // Length
                                0x09,  // AD type: Complete Local Name
                                'T',
                                'e',
                                's',
                                't');

const StaticByteBuffer kInvalidAdvData{
    // 32 bit service UUIDs are supposed to be 4 bytes, but the value in this
    // TLV field is only 3
    // bytes long, hence the AdvertisingData is not valid.
    0x04,
    static_cast<uint8_t>(DataType::kComplete32BitServiceUuids),
    0x01,
    0x02,
    0x03,
};

const bt::sm::LTK kLTK;

const DeviceAddress kAddrLePublic(DeviceAddress::Type::kLEPublic,
                                  {1, 2, 3, 4, 5, 6});
const DeviceAddress kAddrLeRandom(DeviceAddress::Type::kLERandom,
                                  {1, 2, 3, 4, 5, 6});
const DeviceAddress kAddrBrEdr(DeviceAddress::Type::kBREDR,
                               {0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA});
// LE Public Device Address that has the same value as a BR/EDR BD_ADDR, e.g. on
// a dual-mode device.
const DeviceAddress kAddrLeAlias(DeviceAddress::Type::kLEPublic,
                                 {0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA});

const bt::sm::LTK kSecureBrEdrKey(
    sm::SecurityProperties(/*encrypted=*/true,
                           /*authenticated=*/true,
                           /*secure_connections=*/true,
                           sm::kMaxEncryptionKeySize),
    hci_spec::LinkKey(UInt128{4}, 5, 6));
const bt::sm::LTK kLessSecureBrEdrKey(
    sm::SecurityProperties(/*encrypted=*/true,
                           /*authenticated=*/true,
                           /*secure_connections=*/false,
                           sm::kMaxEncryptionKeySize),
    hci_spec::LinkKey(UInt128{4}, 5, 6));
const bt::sm::LTK kSecureBrEdrKey2(
    sm::SecurityProperties(/*encrypted=*/true,
                           /*authenticated=*/true,
                           /*secure_connections=*/true,
                           sm::kMaxEncryptionKeySize),
    hci_spec::LinkKey(UInt128{5}, 6, 7));

class PeerTest : public pw::async::test::FakeDispatcherFixture {
 public:
  PeerTest() = default;

  void SetUp() override {
    // Set up a default peer.
    SetUpPeer(/*address=*/kAddrLePublic, /*connectable=*/true);
  }

  void TearDown() override { peer_.reset(); }

 protected:
  // Can be used to override or reset the default peer. Resets metrics to
  // prevent interference between peers (e.g. by metrics updated in
  // construction).
  void SetUpPeer(const DeviceAddress& address, bool connectable) {
    address_ = address;
    peer_ = std::make_unique<Peer>(
        fit::bind_member<&PeerTest::NotifyListenersCallback>(this),
        fit::bind_member<&PeerTest::UpdateExpiryCallback>(this),
        fit::bind_member<&PeerTest::DualModeCallback>(this),
        fit::bind_member<&PeerTest::StoreLowEnergyBondCallback>(this),
        PeerId(1),
        address_,
        connectable,
        &metrics_,
        dispatcher());
    peer_->AttachInspect(peer_inspector_.GetRoot());
    // Reset metrics as they should only apply to the new peer under test.
    metrics_.AttachInspect(metrics_inspector_.GetRoot());
  }
  Peer& peer() { return *peer_; }

#ifndef NINSPECT
  inspect::Hierarchy ReadPeerInspect() { return ReadInspect(peer_inspector_); }

  std::string InspectLowEnergyConnectionState() {
    std::optional<std::string> val =
        GetInspectValue<inspect::StringPropertyValue>(
            peer_inspector_,
            {"peer",
             "le_data",
             Peer::LowEnergyData::kInspectConnectionStateName});
    PW_CHECK(val);
    return *val;
  }

  int64_t InspectAdvertisingDataParseFailureCount() {
    std::optional<int64_t> val = GetInspectValue<inspect::IntPropertyValue>(
        peer_inspector_,
        {"peer",
         "le_data",
         Peer::LowEnergyData::kInspectAdvertisingDataParseFailureCountName});
    PW_CHECK(val);
    return *val;
  }

  std::string InspectLastAdvertisingDataParseFailure() {
    std::optional<std::string> val =
        GetInspectValue<inspect::StringPropertyValue>(
            peer_inspector_,
            {"peer",
             "le_data",
             Peer::LowEnergyData::kInspectLastAdvertisingDataParseFailureName});
    PW_CHECK(val);
    return *val;
  }

  uint64_t MetricsLowEnergyConnections() {
    std::optional<uint64_t> val = GetInspectValue<inspect::UintPropertyValue>(
        metrics_inspector_, {"metrics", "le", "connection_events"});
    PW_CHECK(val);
    return *val;
  }

  uint64_t MetricsLowEnergyDisconnections() {
    std::optional<uint64_t> val = GetInspectValue<inspect::UintPropertyValue>(
        metrics_inspector_, {"metrics", "le", "disconnection_events"});
    PW_CHECK(val);
    return *val;
  }

  std::string InspectBrEdrConnectionState() {
    std::optional<std::string> val =
        GetInspectValue<inspect::StringPropertyValue>(
            peer_inspector_,
            {"peer",
             "bredr_data",
             Peer::BrEdrData::kInspectConnectionStateName});
    PW_CHECK(val);
    return *val;
  }

  uint64_t MetricsBrEdrConnections() {
    std::optional<uint64_t> val = GetInspectValue<inspect::UintPropertyValue>(
        metrics_inspector_, {"metrics", "bredr", "connection_events"});
    PW_CHECK(val);
    return *val;
  }

  uint64_t MetricsBrEdrDisconnections() {
    std::optional<uint64_t> val = GetInspectValue<inspect::UintPropertyValue>(
        metrics_inspector_, {"metrics", "bredr", "disconnection_events"});
    PW_CHECK(val);
    return *val;
  }
#endif  // NINSPECT

  void set_notify_listeners_cb(Peer::NotifyListenersCallback cb) {
    notify_listeners_cb_ = std::move(cb);
  }
  void set_update_expiry_cb(Peer::PeerCallback cb) {
    update_expiry_cb_ = std::move(cb);
  }
  void set_dual_mode_cb(Peer::PeerCallback cb) {
    dual_mode_cb_ = std::move(cb);
  }
  void set_store_le_bond_cb(Peer::StoreLowEnergyBondCallback cb) {
    store_le_bond_cb_ = std::move(cb);
  }

 private:
  void NotifyListenersCallback(const Peer& peer,
                               Peer::NotifyListenersChange change) {
    if (notify_listeners_cb_) {
      notify_listeners_cb_(peer, change);
    }
  }

  void UpdateExpiryCallback(const Peer& peer) {
    if (update_expiry_cb_) {
      update_expiry_cb_(peer);
    }
  }

  void DualModeCallback(const Peer& peer) {
    if (dual_mode_cb_) {
      dual_mode_cb_(peer);
    }
  }

  bool StoreLowEnergyBondCallback(const sm::PairingData& data) {
    if (store_le_bond_cb_) {
      return store_le_bond_cb_(data);
    }
    return false;
  }

  std::unique_ptr<Peer> peer_;
  DeviceAddress address_;
  Peer::NotifyListenersCallback notify_listeners_cb_;
  Peer::PeerCallback update_expiry_cb_;
  Peer::PeerCallback dual_mode_cb_;
  Peer::StoreLowEnergyBondCallback store_le_bond_cb_;
  inspect::Inspector metrics_inspector_;
  PeerMetrics metrics_;
  inspect::Inspector peer_inspector_;
};

class PeerDeathTest : public PeerTest {};

#ifndef NINSPECT
TEST_F(PeerTest, InspectHierarchy) {
  peer().set_version(pw::bluetooth::emboss::CoreSpecificationVersion::V5_0,
                     kManufacturer,
                     kSubversion);

  peer().RegisterName("Sapphire💖", Peer::NameSource::kGenericAccessService);

  peer().MutLe();
  ASSERT_TRUE(peer().le().has_value());

  peer().MutLe().SetFeatures(hci_spec::LESupportedFeatures{0x0000000000000001});

  peer().MutBrEdr().AddService(UUID(uint16_t{0x110b}));

  auto bredr_data_matcher = AllOf(NodeMatches(
      AllOf(NameMatches(Peer::BrEdrData::kInspectNodeName),
            PropertyList(UnorderedElementsAre(
                StringIs(Peer::BrEdrData::kInspectConnectionStateName,
                         Peer::ConnectionStateToString(
                             peer().bredr()->connection_state())),
                StringIs(Peer::BrEdrData::kInspectServicesName,
                         "{ 0000110b-0000-1000-8000-00805f9b34fb }"))))));

  auto le_data_matcher = AllOf(NodeMatches(AllOf(
      NameMatches(Peer::LowEnergyData::kInspectNodeName),
      PropertyList(UnorderedElementsAre(
          StringIs(
              Peer::LowEnergyData::kInspectConnectionStateName,
              Peer::ConnectionStateToString(peer().le()->connection_state())),
          IntIs(
              Peer::LowEnergyData::kInspectAdvertisingDataParseFailureCountName,
              0),
          StringIs(
              Peer::LowEnergyData::kInspectLastAdvertisingDataParseFailureName,
              ""),
          BoolIs(Peer::LowEnergyData::kInspectBondDataName,
                 peer().le()->bonded()),
          StringIs(Peer::LowEnergyData::kInspectFeaturesName,
                   "0x0000000000000001"))))));

  auto peer_matcher = AllOf(
      NodeMatches(PropertyList(UnorderedElementsAre(
          StringIs(Peer::kInspectPeerIdName, peer().identifier().ToString()),
          StringIs(Peer::kInspectPeerNameName,
                   peer().name().value() + " [source: " +
                       Peer::NameSourceToString(
                           Peer::NameSource::kGenericAccessService) +
                       "]"),
          StringIs(Peer::kInspectTechnologyName,
                   TechnologyTypeToString(peer().technology())),
          StringIs(Peer::kInspectAddressName, peer().address().ToString()),
          BoolIs(Peer::kInspectConnectableName, peer().connectable()),
          BoolIs(Peer::kInspectTemporaryName, peer().temporary()),
          StringIs(Peer::kInspectFeaturesName, peer().features().ToString()),
          StringIs(Peer::kInspectVersionName,
                   hci_spec::HCIVersionToString(peer().version().value())),
          StringIs(Peer::kInspectManufacturerName,
                   GetManufacturerName(kManufacturer))))),
      ChildrenMatch(UnorderedElementsAre(bredr_data_matcher, le_data_matcher)));
  // clang-format on
  inspect::Hierarchy hierarchy = ReadPeerInspect();
  EXPECT_THAT(hierarchy,
              AllOf(ChildrenMatch(UnorderedElementsAre(peer_matcher))));
}
#endif  // NINSPECT

#ifndef NINSPECT
TEST_F(PeerTest, SetBrEdrBondDataUpdatesInspectProperties) {
  const char* const kInspectLevelPropertyName = "level";
  const char* const kInspectEncryptedPropertyName = "encrypted";
  const char* const kInspectSecureConnectionsPropertyName =
      "secure_connections";
  const char* const kInspectAuthenticatedPropertyName = "authenticated";
  const char* const kInspectKeyTypePropertyName = "key_type";

  peer().set_version(pw::bluetooth::emboss::CoreSpecificationVersion::V5_0,
                     kManufacturer,
                     kSubversion);

  peer().RegisterName("Sapphire💖", Peer::NameSource::kGenericAccessService);

  peer().MutLe();
  ASSERT_TRUE(peer().le().has_value());

  peer().MutLe().SetFeatures(hci_spec::LESupportedFeatures{0x0000000000000001});

  peer().MutBrEdr().AddService(UUID(uint16_t{0x110b}));
  EXPECT_TRUE(peer().MutBrEdr().SetBondData(kLTK));

  // clang-format off
  auto bredr_data_matcher = AllOf(
    NodeMatches(AllOf(
      NameMatches(Peer::BrEdrData::kInspectNodeName),
      PropertyList(UnorderedElementsAre(
        StringIs(Peer::BrEdrData::kInspectConnectionStateName,
                 Peer::ConnectionStateToString(peer().bredr()->connection_state())),
        StringIs(Peer::BrEdrData::kInspectServicesName, "{ 0000110b-0000-1000-8000-00805f9b34fb }")
        )))));

  auto le_data_matcher = AllOf(
    NodeMatches(AllOf(
      NameMatches(Peer::LowEnergyData::kInspectNodeName),
      PropertyList(UnorderedElementsAre(
        StringIs(Peer::LowEnergyData::kInspectConnectionStateName,
                 Peer::ConnectionStateToString(peer().le()->connection_state())),
        IntIs(Peer::LowEnergyData::kInspectAdvertisingDataParseFailureCountName, 0),
        StringIs(Peer::LowEnergyData::kInspectLastAdvertisingDataParseFailureName, ""),
        BoolIs(Peer::LowEnergyData::kInspectBondDataName, peer().le()->bonded()),
        StringIs(Peer::LowEnergyData::kInspectFeaturesName, "0x0000000000000001")
        )))));

  auto peer_matcher = AllOf(
    NodeMatches(
      PropertyList(UnorderedElementsAre(
        StringIs(Peer::kInspectPeerIdName, peer().identifier().ToString()),
        StringIs(Peer::kInspectPeerNameName, peer().name().value() + " [source: " + Peer::NameSourceToString(Peer::NameSource::kGenericAccessService) + "]"),
        StringIs(Peer::kInspectTechnologyName, TechnologyTypeToString(peer().technology())),
        StringIs(Peer::kInspectAddressName, peer().address().ToString()),
        BoolIs(Peer::kInspectConnectableName, peer().connectable()),
        BoolIs(Peer::kInspectTemporaryName, peer().temporary()),
        StringIs(Peer::kInspectFeaturesName, peer().features().ToString()),
        StringIs(Peer::kInspectVersionName, hci_spec::HCIVersionToString(peer().version().value())),
        StringIs(Peer::kInspectManufacturerName, GetManufacturerName(kManufacturer))
        ))),
    ChildrenMatch(UnorderedElementsAre(bredr_data_matcher, le_data_matcher)));
  // clang-format on
  inspect::Hierarchy hierarchy = ReadPeerInspect();
  EXPECT_THAT(hierarchy,
              AllOf(ChildrenMatch(UnorderedElementsAre(peer_matcher))));

  EXPECT_TRUE(peer().MutBrEdr().SetBondData(kSecureBrEdrKey));

  const sm::SecurityProperties security_properties =
      peer().bredr()->link_key().value().security();
  auto link_key_matcher = AllOf(NodeMatches(
      AllOf(NameMatches("link_key"),
            PropertyList(UnorderedElementsAre(
                StringIs(kInspectLevelPropertyName,
                         LevelToString(security_properties.level())),
                BoolIs(kInspectEncryptedPropertyName,
                       security_properties.encrypted()),
                BoolIs(kInspectSecureConnectionsPropertyName,
                       security_properties.secure_connections()),
                BoolIs(kInspectAuthenticatedPropertyName,
                       security_properties.authenticated()),
                StringIs(kInspectKeyTypePropertyName,
                         hci_spec::LinkKeyTypeToString(
                             security_properties.GetLinkKeyType())))))));

  auto bredr_data_matcher2 =
      AllOf(NodeMatches(AllOf(
                NameMatches(Peer::BrEdrData::kInspectNodeName),
                PropertyList(UnorderedElementsAre(
                    StringIs(Peer::BrEdrData::kInspectConnectionStateName,
                             Peer::ConnectionStateToString(
                                 peer().bredr()->connection_state())),
                    StringIs(Peer::BrEdrData::kInspectServicesName,
                             "{ 0000110b-0000-1000-8000-00805f9b34fb }"))))),
            ChildrenMatch(UnorderedElementsAre(link_key_matcher)));

  auto le_data_matcher2 = AllOf(NodeMatches(AllOf(
      NameMatches(Peer::LowEnergyData::kInspectNodeName),
      PropertyList(UnorderedElementsAre(
          StringIs(
              Peer::LowEnergyData::kInspectConnectionStateName,
              Peer::ConnectionStateToString(peer().le()->connection_state())),
          IntIs(
              Peer::LowEnergyData::kInspectAdvertisingDataParseFailureCountName,
              0),
          StringIs(
              Peer::LowEnergyData::kInspectLastAdvertisingDataParseFailureName,
              ""),
          BoolIs(Peer::LowEnergyData::kInspectBondDataName,
                 peer().le()->bonded()),
          StringIs(Peer::LowEnergyData::kInspectFeaturesName,
                   "0x0000000000000001"))))));

  auto peer_matcher2 = AllOf(
      NodeMatches(PropertyList(UnorderedElementsAre(
          StringIs(Peer::kInspectPeerIdName, peer().identifier().ToString()),
          StringIs(Peer::kInspectPeerNameName,
                   peer().name().value() + " [source: " +
                       Peer::NameSourceToString(
                           Peer::NameSource::kGenericAccessService) +
                       "]"),
          StringIs(Peer::kInspectTechnologyName,
                   TechnologyTypeToString(peer().technology())),
          StringIs(Peer::kInspectAddressName, peer().address().ToString()),
          BoolIs(Peer::kInspectConnectableName, peer().connectable()),
          BoolIs(Peer::kInspectTemporaryName, peer().temporary()),
          StringIs(Peer::kInspectFeaturesName, peer().features().ToString()),
          StringIs(Peer::kInspectVersionName,
                   hci_spec::HCIVersionToString(peer().version().value())),
          StringIs(Peer::kInspectManufacturerName,
                   GetManufacturerName(kManufacturer))))),
      ChildrenMatch(
          UnorderedElementsAre(bredr_data_matcher2, le_data_matcher2)));
  // clang-format on
  hierarchy = ReadPeerInspect();
  EXPECT_THAT(hierarchy,
              AllOf(ChildrenMatch(UnorderedElementsAre(peer_matcher2))));
}
#endif  // NINSPECT

TEST_F(PeerTest, BrEdrDataAddServiceNotifiesListeners) {
  // Initialize BrEdrData.
  peer().MutBrEdr();
  ASSERT_TRUE(peer().bredr()->services().empty());

  bool listener_notified = false;
  set_notify_listeners_cb([&](auto&, Peer::NotifyListenersChange change) {
    listener_notified = true;
    // Non-bonded peer should not update bond
    EXPECT_EQ(Peer::NotifyListenersChange::kBondNotUpdated, change);
  });

  constexpr UUID kServiceUuid;
  peer().MutBrEdr().AddService(kServiceUuid);
  EXPECT_TRUE(listener_notified);
  EXPECT_EQ(1u, peer().bredr()->services().count(kServiceUuid));

  // De-duplicate subsequent additions of the same service.
  listener_notified = false;
  peer().MutBrEdr().AddService(kServiceUuid);
  EXPECT_FALSE(listener_notified);
}

TEST_F(PeerTest, BrEdrDataAddServiceOnBondedPeerNotifiesListenersToUpdateBond) {
  // Initialize BrEdrData.
  EXPECT_TRUE(peer().MutBrEdr().SetBondData({}));
  ASSERT_TRUE(peer().bredr()->services().empty());

  bool listener_notified = false;
  set_notify_listeners_cb([&](auto&, Peer::NotifyListenersChange change) {
    listener_notified = true;
    // Bonded peer should update bond
    EXPECT_EQ(Peer::NotifyListenersChange::kBondUpdated, change);
  });

  peer().MutBrEdr().AddService(UUID());
  EXPECT_TRUE(listener_notified);
}

TEST_F(PeerTest,
       LowEnergyDataSetAdvDataWithInvalidUtf8NameDoesNotUpdatePeerName) {
  peer().MutLe();  // Initialize LowEnergyData.
  ASSERT_FALSE(peer().name().has_value());

  bool listener_notified = false;
  set_notify_listeners_cb(
      [&](auto&, Peer::NotifyListenersChange) { listener_notified = true; });

  const StaticByteBuffer kBadAdvData(
      0x05,  // Length
      0x09,  // AD type: Complete Local Name
      'T',
      'e',
      's',
      0xFF  // 0xFF should not appear in a valid UTF-8 string
  );

  peer().MutLe().SetAdvertisingData(
      /*rssi=*/0, kBadAdvData, pw::chrono::SystemClock::time_point());
  EXPECT_TRUE(listener_notified);  // Fresh AD still results in an update
  EXPECT_FALSE(peer().name().has_value());
}

TEST_F(PeerTest, BrEdrDataSetEirDataWithInvalidUtf8NameDoesNotUpdatePeerName) {
  peer().MutBrEdr();  // Initialize BrEdrData.
  ASSERT_FALSE(peer().name().has_value());

  bool listener_notified = false;
  set_notify_listeners_cb(
      [&](auto&, Peer::NotifyListenersChange) { listener_notified = true; });

  const StaticByteBuffer kEirData(
      0x05,  // Length
      0x09,  // AD type: Complete Local Name
      'T',
      'e',
      's',
      0xFF  // 0xFF should not appear in a valid UTF-8 string
  );

  StaticPacket<pw::bluetooth::emboss::ExtendedInquiryResultEventWriter> eirep;
  eirep.view().num_responses().Write(1);
  eirep.view().bd_addr().CopyFrom(peer().address().value().view());
  eirep.view().extended_inquiry_response().BackingStorage().CopyFrom(
      ::emboss::support::ReadOnlyContiguousBuffer(&kEirData), kEirData.size());

  peer().MutBrEdr().SetInquiryData(eirep.view());
  EXPECT_TRUE(listener_notified);  // Fresh EIR data still results in an update
  EXPECT_FALSE(peer().name().has_value());
}

TEST_F(PeerTest, RegisterNameWithInvalidUtf8NameDoesNotUpdatePeerName) {
  ASSERT_FALSE(peer().name().has_value());

  bool listener_notified = false;
  set_notify_listeners_cb(
      [&](auto&, Peer::NotifyListenersChange) { listener_notified = true; });

  const std::string kName =
      "Tes\xFF\x01";  // 0xFF should not appear in a valid UTF-8 string
  peer().RegisterName(kName);
  EXPECT_FALSE(listener_notified);
  EXPECT_FALSE(peer().name().has_value());
}

TEST_F(PeerTest, LowEnergyAdvertisingDataTimestamp) {
  EXPECT_FALSE(peer().MutLe().parsed_advertising_data_timestamp());
  peer().MutLe().SetAdvertisingData(
      /*rssi=*/0,
      kAdvData,
      pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(1)));
  ASSERT_TRUE(peer().MutLe().parsed_advertising_data_timestamp());
  EXPECT_EQ(peer().MutLe().parsed_advertising_data_timestamp().value(),
            pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(1)));

  peer().MutLe().SetAdvertisingData(
      /*rssi=*/0,
      kAdvData,
      pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(2)));
  ASSERT_TRUE(peer().MutLe().parsed_advertising_data_timestamp());
  EXPECT_EQ(peer().MutLe().parsed_advertising_data_timestamp().value(),
            pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(2)));

  // SetAdvertisingData with data that fails to parse should not update the
  // advertising data timestamp.
  peer().MutLe().SetAdvertisingData(
      /*rssi=*/0,
      kInvalidAdvData,
      pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(3)));
  ASSERT_TRUE(peer().MutLe().parsed_advertising_data_timestamp());
  EXPECT_EQ(peer().MutLe().parsed_advertising_data_timestamp().value(),
            pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(2)));
}

TEST_F(PeerTest, SettingLowEnergyAdvertisingDataUpdatesLastUpdated) {
  EXPECT_EQ(peer().last_updated(),
            pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(0)));

  int notify_count = 0;
  set_notify_listeners_cb([&](const Peer&, Peer::NotifyListenersChange) {
    EXPECT_EQ(peer().last_updated(),
              pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(2)));
    notify_count++;
  });

  RunFor(pw::chrono::SystemClock::duration(2));
  peer().MutLe().SetAdvertisingData(
      /*rssi=*/0,
      kAdvData,
      pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(1)));
  EXPECT_EQ(peer().last_updated(),
            pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(2)));
  EXPECT_GE(notify_count, 1);
}

TEST_F(PeerTest, RegisteringLowEnergyInitializingConnectionUpdatesLastUpdated) {
  EXPECT_EQ(peer().last_updated(),
            pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(0)));

  int notify_count = 0;
  set_notify_listeners_cb([&](const Peer&, Peer::NotifyListenersChange) {
    EXPECT_EQ(peer().last_updated(),
              pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(2)));
    notify_count++;
  });

  RunFor(pw::chrono::SystemClock::duration(2));
  Peer::InitializingConnectionToken token =
      peer().MutLe().RegisterInitializingConnection();
  EXPECT_EQ(peer().last_updated(),
            pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(2)));
  EXPECT_GE(notify_count, 1);
}

TEST_F(PeerTest, SettingLowEnergyBondDataUpdatesLastUpdated) {
  EXPECT_EQ(peer().last_updated(),
            pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(0)));

  int notify_count = 0;
  set_notify_listeners_cb([&](const Peer&, Peer::NotifyListenersChange) {
    EXPECT_EQ(peer().last_updated(),
              pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(2)));
    notify_count++;
  });

  RunFor(pw::chrono::SystemClock::duration(2));
  sm::PairingData data;
  data.peer_ltk = kLTK;
  data.local_ltk = kLTK;
  peer().MutLe().SetBondData(data);
  EXPECT_EQ(peer().last_updated(),
            pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(2)));
  EXPECT_GE(notify_count, 1);
}

TEST_F(PeerTest, RegisteringBrEdrInitializingConnectionUpdatesLastUpdated) {
  EXPECT_EQ(peer().last_updated(),
            pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(0)));

  int notify_count = 0;
  set_notify_listeners_cb([&](const Peer&, Peer::NotifyListenersChange) {
    EXPECT_EQ(peer().last_updated(),
              pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(2)));
    notify_count++;
  });

  RunFor(pw::chrono::SystemClock::duration(2));
  Peer::InitializingConnectionToken token =
      peer().MutBrEdr().RegisterInitializingConnection();
  EXPECT_EQ(peer().last_updated(),
            pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(2)));
  EXPECT_GE(notify_count, 1);
}

TEST_F(PeerTest, SettingInquiryDataUpdatesLastUpdated) {
  SetUpPeer(/*address=*/kAddrLeAlias, /*connectable=*/true);
  EXPECT_EQ(peer().last_updated(),
            pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(0)));

  int notify_count = 0;
  set_notify_listeners_cb([&](const Peer&, Peer::NotifyListenersChange) {
    EXPECT_EQ(peer().last_updated(),
              pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(2)));
    notify_count++;
  });

  RunFor(pw::chrono::SystemClock::duration(2));
  StaticPacket<pw::bluetooth::emboss::InquiryResultWriter> ir;
  ir.view().bd_addr().CopyFrom(kAddrLeAlias.value().view());
  peer().MutBrEdr().SetInquiryData(ir.view());
  EXPECT_EQ(peer().last_updated(),
            pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(2)));
  EXPECT_GE(notify_count, 1);
}

TEST_F(PeerTest, SettingBrEdrBondDataUpdatesLastUpdated) {
  EXPECT_EQ(peer().last_updated(),
            pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(0)));

  int notify_count = 0;
  set_notify_listeners_cb([&](const Peer&, Peer::NotifyListenersChange) {
    EXPECT_EQ(peer().last_updated(),
              pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(2)));
    notify_count++;
  });

  RunFor(pw::chrono::SystemClock::duration(2));
  EXPECT_TRUE(peer().MutBrEdr().SetBondData(kSecureBrEdrKey));
  EXPECT_EQ(peer().last_updated(),
            pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(2)));
  EXPECT_GE(notify_count, 1);
}

TEST_F(PeerTest, BrEdrDataSetDeviceClassNotifiesListeners) {
  // Initialize BrEdrData.
  peer().MutBrEdr();
  ASSERT_FALSE(peer().bredr()->device_class().has_value());
  EXPECT_EQ(peer().last_updated(),
            pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(0)));

  bool listener_notified = false;
  set_notify_listeners_cb([&](auto&, Peer::NotifyListenersChange change) {
    listener_notified = true;
    EXPECT_EQ(Peer::NotifyListenersChange::kBondNotUpdated, change);
    EXPECT_EQ(peer().last_updated(),
              pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(2)));
  });

  RunFor(pw::chrono::SystemClock::duration(2));
  DeviceClass kDeviceClass(0x123456);
  peer().MutBrEdr().SetDeviceClass(kDeviceClass);
  EXPECT_TRUE(listener_notified);
  ASSERT_TRUE(peer().bredr()->device_class().has_value());
  EXPECT_EQ(kDeviceClass, *peer().bredr()->device_class());

  // Subsequent setting of the same device class should not notify.
  listener_notified = false;
  peer().MutBrEdr().SetDeviceClass(kDeviceClass);
  EXPECT_FALSE(listener_notified);

  // Changing device class should notify.
  listener_notified = false;
  DeviceClass kNewDeviceClass(0x654321);
  peer().MutBrEdr().SetDeviceClass(kNewDeviceClass);
  EXPECT_TRUE(listener_notified);
  EXPECT_EQ(kNewDeviceClass, *peer().bredr()->device_class());
}

TEST_F(PeerTest, SettingAddingBrEdrServiceUpdatesLastUpdated) {
  EXPECT_EQ(peer().last_updated(),
            pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(0)));

  int notify_count = 0;
  set_notify_listeners_cb([&](const Peer&, Peer::NotifyListenersChange) {
    EXPECT_EQ(peer().last_updated(),
              pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(2)));
    notify_count++;
  });

  RunFor(pw::chrono::SystemClock::duration(2));
  peer().MutBrEdr().AddService(UUID(uint16_t{0x110b}));
  EXPECT_EQ(peer().last_updated(),
            pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(2)));
  EXPECT_GE(notify_count, 1);
}

TEST_F(PeerTest, RegisteringNameUpdatesLastUpdated) {
  EXPECT_EQ(peer().last_updated(),
            pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(0)));

  int notify_count = 0;
  set_notify_listeners_cb([&](const Peer&, Peer::NotifyListenersChange) {
    EXPECT_EQ(peer().last_updated(),
              pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(2)));
    notify_count++;
  });

  RunFor(pw::chrono::SystemClock::duration(2));
  peer().RegisterName("name");
  EXPECT_EQ(peer().last_updated(),
            pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(2)));
  EXPECT_GE(notify_count, 1);
}

TEST_F(PeerTest, RegisterAndUnregisterTwoLowEnergyConnections) {
  SetUpPeer(/*address=*/kAddrLeRandom, /*connectable=*/true);

  int update_expiry_count = 0;
  set_update_expiry_cb([&](const Peer&) { update_expiry_count++; });
  int notify_count = 0;
  set_notify_listeners_cb(
      [&](const Peer&, Peer::NotifyListenersChange) { notify_count++; });

  std::optional<Peer::ConnectionToken> token_0 =
      peer().MutLe().RegisterConnection();
  // A notification and expiry update are sent when the peer becomes
  // non-temporary, and a second notification and update expiry are sent because
  // the connection is registered.
  EXPECT_EQ(update_expiry_count, 2);
  EXPECT_EQ(notify_count, 2);
  EXPECT_FALSE(peer().temporary());
  EXPECT_EQ(peer().le()->connection_state(), Peer::ConnectionState::kConnected);
#ifndef NINSPECT
  EXPECT_EQ(InspectLowEnergyConnectionState(),
            Peer::ConnectionStateToString(Peer::ConnectionState::kConnected));
  EXPECT_EQ(MetricsLowEnergyConnections(), 1u);
#endif  // NINSPECT

  std::optional<Peer::ConnectionToken> token_1 =
      peer().MutLe().RegisterConnection();
  // The second connection should not update expiry or notify.
  EXPECT_EQ(update_expiry_count, 2);
  EXPECT_EQ(notify_count, 2);
  EXPECT_FALSE(peer().temporary());
  EXPECT_EQ(peer().le()->connection_state(), Peer::ConnectionState::kConnected);
#ifndef NINSPECT
  EXPECT_EQ(InspectLowEnergyConnectionState(),
            Peer::ConnectionStateToString(Peer::ConnectionState::kConnected));
  // Although the second connection does not change the high-level connection
  // state, we track it in metrics to support multiple connections to the same
  // peer.
  EXPECT_EQ(MetricsLowEnergyConnections(), 2u);
  EXPECT_EQ(MetricsLowEnergyDisconnections(), 0u);
#endif  // NINSPECT

  token_0.reset();
  EXPECT_EQ(update_expiry_count, 2);
  EXPECT_EQ(notify_count, 2);
  EXPECT_FALSE(peer().temporary());
  EXPECT_EQ(peer().le()->connection_state(), Peer::ConnectionState::kConnected);
#ifndef NINSPECT
  EXPECT_EQ(InspectLowEnergyConnectionState(),
            Peer::ConnectionStateToString(Peer::ConnectionState::kConnected));
  EXPECT_EQ(MetricsLowEnergyDisconnections(), 1u);
#endif  // NINSPECT

  token_1.reset();
  EXPECT_EQ(update_expiry_count, 3);
  EXPECT_EQ(notify_count, 3);
  EXPECT_TRUE(peer().temporary());
  EXPECT_EQ(peer().le()->connection_state(),
            Peer::ConnectionState::kNotConnected);
#ifndef NINSPECT
  EXPECT_EQ(
      InspectLowEnergyConnectionState(),
      Peer::ConnectionStateToString(Peer::ConnectionState::kNotConnected));
  EXPECT_EQ(MetricsLowEnergyDisconnections(), 2u);
#endif  // NINSPECT
}

TEST_F(PeerTest, RegisterAndUnregisterLowEnergyConnectionsWhenIdentityKnown) {
  EXPECT_TRUE(peer().identity_known());
  std::optional<Peer::ConnectionToken> token =
      peer().MutLe().RegisterConnection();
  EXPECT_FALSE(peer().temporary());
  token.reset();
  // The peer's identity is known, so it should stay non-temporary upon
  // disconnection.
  EXPECT_FALSE(peer().temporary());
  EXPECT_EQ(peer().le()->connection_state(),
            Peer::ConnectionState::kNotConnected);
#ifndef NINSPECT
  EXPECT_EQ(
      InspectLowEnergyConnectionState(),
      Peer::ConnectionStateToString(Peer::ConnectionState::kNotConnected));
#endif  // NINSPECT
}

TEST_F(PeerTest,
       RegisterAndUnregisterInitializingLowEnergyConnectionsWhenIdentityKnown) {
  EXPECT_TRUE(peer().identity_known());
  std::optional<Peer::InitializingConnectionToken> token =
      peer().MutLe().RegisterInitializingConnection();
  EXPECT_FALSE(peer().temporary());
  token.reset();
  // The peer's identity is known, so it should stay non-temporary upon
  // disconnection.
  EXPECT_FALSE(peer().temporary());
  EXPECT_EQ(peer().le()->connection_state(),
            Peer::ConnectionState::kNotConnected);
#ifndef NINSPECT
  EXPECT_EQ(
      InspectLowEnergyConnectionState(),
      Peer::ConnectionStateToString(Peer::ConnectionState::kNotConnected));
#endif  // NINSPECT
}

TEST_F(PeerTest,
       RegisterAndUnregisterLowEnergyConnectionDuringInitializingConnection) {
  SetUpPeer(/*address=*/kAddrLeRandom, /*connectable=*/true);

  int update_expiry_count = 0;
  set_update_expiry_cb([&](const Peer&) { update_expiry_count++; });
  int notify_count = 0;
  set_notify_listeners_cb(
      [&](const Peer&, Peer::NotifyListenersChange) { notify_count++; });

  std::optional<Peer::InitializingConnectionToken> init_token =
      peer().MutLe().RegisterInitializingConnection();
  // A notification and expiry update are sent when the peer becomes
  // non-temporary, and a second notification and update expiry are sent because
  // the initializing connection is registered.
  EXPECT_EQ(update_expiry_count, 2);
  EXPECT_EQ(notify_count, 2);
  EXPECT_FALSE(peer().temporary());
  EXPECT_EQ(peer().le()->connection_state(),
            Peer::ConnectionState::kInitializing);
#ifndef NINSPECT
  EXPECT_EQ(
      InspectLowEnergyConnectionState(),
      Peer::ConnectionStateToString(Peer::ConnectionState::kInitializing));
#endif  // NINSPECT

  std::optional<Peer::ConnectionToken> conn_token =
      peer().MutLe().RegisterConnection();
  EXPECT_EQ(update_expiry_count, 3);
  EXPECT_EQ(notify_count, 3);
  EXPECT_FALSE(peer().temporary());
  EXPECT_EQ(peer().le()->connection_state(), Peer::ConnectionState::kConnected);
#ifndef NINSPECT
  EXPECT_EQ(InspectLowEnergyConnectionState(),
            Peer::ConnectionStateToString(Peer::ConnectionState::kConnected));
#endif  // NINSPECT

  conn_token.reset();
  EXPECT_EQ(update_expiry_count, 4);
  EXPECT_EQ(notify_count, 4);
  EXPECT_FALSE(peer().temporary());
  EXPECT_EQ(peer().le()->connection_state(),
            Peer::ConnectionState::kInitializing);
#ifndef NINSPECT
  EXPECT_EQ(
      InspectLowEnergyConnectionState(),
      Peer::ConnectionStateToString(Peer::ConnectionState::kInitializing));
#endif  // NINSPECT

  init_token.reset();
  EXPECT_EQ(update_expiry_count, 5);
  EXPECT_EQ(notify_count, 5);
  EXPECT_TRUE(peer().temporary());
  EXPECT_EQ(peer().le()->connection_state(),
            Peer::ConnectionState::kNotConnected);
#ifndef NINSPECT
  EXPECT_EQ(
      InspectLowEnergyConnectionState(),
      Peer::ConnectionStateToString(Peer::ConnectionState::kNotConnected));
#endif  // NINSPECT
}

TEST_F(PeerTest,
       RegisterAndUnregisterInitializingLowEnergyConnectionDuringConnection) {
  SetUpPeer(/*address=*/kAddrLeRandom, /*connectable=*/true);

  int update_expiry_count = 0;
  set_update_expiry_cb([&](const Peer&) { update_expiry_count++; });
  int notify_count = 0;
  set_notify_listeners_cb(
      [&](const Peer&, Peer::NotifyListenersChange) { notify_count++; });

  std::optional<Peer::ConnectionToken> conn_token =
      peer().MutLe().RegisterConnection();
  // A notification and expiry update are sent when the peer becomes
  // non-temporary, and a second notification and update expiry are sent because
  // the initializing connection is registered.
  EXPECT_EQ(update_expiry_count, 2);
  EXPECT_EQ(notify_count, 2);
  EXPECT_FALSE(peer().temporary());
  EXPECT_EQ(peer().le()->connection_state(), Peer::ConnectionState::kConnected);
#ifndef NINSPECT
  EXPECT_EQ(InspectLowEnergyConnectionState(),
            Peer::ConnectionStateToString(Peer::ConnectionState::kConnected));
#endif  // NINSPECT

  std::optional<Peer::InitializingConnectionToken> init_token =
      peer().MutLe().RegisterInitializingConnection();
  // Initializing connections should not affect the expiry or notify listeners
  // for peers that are already connected.
  EXPECT_EQ(update_expiry_count, 2);
  EXPECT_EQ(notify_count, 2);
  EXPECT_FALSE(peer().temporary());
  EXPECT_EQ(peer().le()->connection_state(), Peer::ConnectionState::kConnected);
#ifndef NINSPECT
  EXPECT_EQ(InspectLowEnergyConnectionState(),
            Peer::ConnectionStateToString(Peer::ConnectionState::kConnected));
#endif  // NINSPECT

  init_token.reset();
  EXPECT_EQ(update_expiry_count, 2);
  EXPECT_EQ(notify_count, 2);
  EXPECT_FALSE(peer().temporary());
  EXPECT_EQ(peer().le()->connection_state(), Peer::ConnectionState::kConnected);
#ifndef NINSPECT
  EXPECT_EQ(InspectLowEnergyConnectionState(),
            Peer::ConnectionStateToString(Peer::ConnectionState::kConnected));
#endif  // NINSPECT

  conn_token.reset();
  EXPECT_EQ(update_expiry_count, 3);
  EXPECT_EQ(notify_count, 3);
  EXPECT_TRUE(peer().temporary());
  EXPECT_EQ(peer().le()->connection_state(),
            Peer::ConnectionState::kNotConnected);
#ifndef NINSPECT
  EXPECT_EQ(
      InspectLowEnergyConnectionState(),
      Peer::ConnectionStateToString(Peer::ConnectionState::kNotConnected));
#endif  // NINSPECT
}

TEST_F(PeerTest, RegisterAndUnregisterTwoLowEnergyInitializingConnections) {
  SetUpPeer(/*address=*/kAddrLeRandom, /*connectable=*/true);

  int update_expiry_count = 0;
  set_update_expiry_cb([&](const Peer&) { update_expiry_count++; });
  int notify_count = 0;
  set_notify_listeners_cb(
      [&](const Peer&, Peer::NotifyListenersChange) { notify_count++; });

  std::optional<Peer::InitializingConnectionToken> token_0 =
      peer().MutLe().RegisterInitializingConnection();
  // A notification and expiry update are sent when the peer becomes
  // non-temporary, and a second notification and update expiry are sent because
  // the initializing connection is registered.
  EXPECT_EQ(update_expiry_count, 2);
  EXPECT_EQ(notify_count, 2);
  EXPECT_FALSE(peer().temporary());
  EXPECT_EQ(peer().le()->connection_state(),
            Peer::ConnectionState::kInitializing);
#ifndef NINSPECT
  EXPECT_EQ(
      InspectLowEnergyConnectionState(),
      Peer::ConnectionStateToString(Peer::ConnectionState::kInitializing));
#endif  // NINSPECT

  std::optional<Peer::InitializingConnectionToken> token_1 =
      peer().MutLe().RegisterInitializingConnection();
  // The second initializing connection should not update expiry or notify.
  EXPECT_EQ(update_expiry_count, 2);
  EXPECT_EQ(notify_count, 2);
  EXPECT_FALSE(peer().temporary());
  EXPECT_EQ(peer().le()->connection_state(),
            Peer::ConnectionState::kInitializing);
#ifndef NINSPECT
  EXPECT_EQ(
      InspectLowEnergyConnectionState(),
      Peer::ConnectionStateToString(Peer::ConnectionState::kInitializing));
#endif  // NINSPECT

  token_0.reset();
  EXPECT_EQ(update_expiry_count, 2);
  EXPECT_EQ(notify_count, 2);
  EXPECT_FALSE(peer().temporary());
  EXPECT_EQ(peer().le()->connection_state(),
            Peer::ConnectionState::kInitializing);
#ifndef NINSPECT
  EXPECT_EQ(
      InspectLowEnergyConnectionState(),
      Peer::ConnectionStateToString(Peer::ConnectionState::kInitializing));
#endif  // NINSPECT
  token_1.reset();
  EXPECT_EQ(update_expiry_count, 3);
  EXPECT_EQ(notify_count, 3);
  // The peer's identity is not known, so it should become temporary upon
  // disconnection.
  EXPECT_TRUE(peer().temporary());
  EXPECT_EQ(peer().le()->connection_state(),
            Peer::ConnectionState::kNotConnected);
#ifndef NINSPECT
  EXPECT_EQ(
      InspectLowEnergyConnectionState(),
      Peer::ConnectionStateToString(Peer::ConnectionState::kNotConnected));
#endif  // NINSPECT
}

TEST_F(PeerTest, MovingLowEnergyConnectionTokenWorksAsExpected) {
  std::optional<Peer::ConnectionToken> token_0 =
      peer().MutLe().RegisterConnection();
  EXPECT_EQ(peer().le()->connection_state(), Peer::ConnectionState::kConnected);

  std::optional<Peer::ConnectionToken> token_1 = std::move(token_0);
  EXPECT_EQ(peer().le()->connection_state(), Peer::ConnectionState::kConnected);

  token_0.reset();
  EXPECT_EQ(peer().le()->connection_state(), Peer::ConnectionState::kConnected);

  token_1.reset();
  EXPECT_EQ(peer().le()->connection_state(),
            Peer::ConnectionState::kNotConnected);
}

TEST_F(PeerTest, RegisterNamesWithVariousSources) {
  ASSERT_FALSE(peer().name().has_value());
  ASSERT_TRUE(
      peer().RegisterName("test", Peer::NameSource::kAdvertisingDataComplete));

  // Test that name with lower source priority does not replace stored name with
  // higher priority.
  ASSERT_FALSE(peer().RegisterName("test", Peer::NameSource::kUnknown));

  // Test that name with higher source priority replaces stored name with lower
  // priority.
  ASSERT_TRUE(
      peer().RegisterName("test", Peer::NameSource::kGenericAccessService));

  // Test that stored name is not replaced with an identical name from an
  // identical source.
  ASSERT_FALSE(
      peer().RegisterName("test", Peer::NameSource::kGenericAccessService));

  // Test that stored name is replaced by a different name from the same source.
  ASSERT_TRUE(peer().RegisterName("different_name",
                                  Peer::NameSource::kGenericAccessService));
}

TEST_F(PeerTest, SetValidAdvertisingData) {
  constexpr const char* kLocalName = "Test";
  StaticByteBuffer raw_data{
      // Length - Type - Value formatted Local name
      0x05,
      static_cast<uint8_t>(DataType::kCompleteLocalName),
      kLocalName[0],
      kLocalName[1],
      kLocalName[2],
      kLocalName[3],
  };
  peer().MutLe().SetAdvertisingData(
      /*rssi=*/32, raw_data, pw::chrono::SystemClock::time_point());
  // Setting an AdvertisingData with a local name field should update the peer's
  // local name.
  ASSERT_TRUE(peer().name().has_value());
  EXPECT_EQ(kLocalName, peer().name().value());
  EXPECT_EQ(Peer::NameSource::kAdvertisingDataComplete, peer().name_source());
#ifndef NINSPECT
  EXPECT_EQ(0, InspectAdvertisingDataParseFailureCount());
  EXPECT_EQ("", InspectLastAdvertisingDataParseFailure());
#endif  // NINSPECT
}

TEST_F(PeerTest, SetShortenedLocalName) {
  constexpr const char* kLocalName = "Test";
  StaticByteBuffer raw_data{
      // Length - Type - Value formatted Local name
      0x05,
      static_cast<uint8_t>(DataType::kShortenedLocalName),
      kLocalName[0],
      kLocalName[1],
      kLocalName[2],
      kLocalName[3],
  };
  peer().MutLe().SetAdvertisingData(
      /*rssi=*/32, raw_data, pw::chrono::SystemClock::time_point());
  ASSERT_TRUE(peer().name().has_value());
  EXPECT_EQ(kLocalName, peer().name().value());
  EXPECT_EQ(Peer::NameSource::kAdvertisingDataShortened, peer().name_source());
  EXPECT_EQ(peer().MutLe().advertising_data().size(), raw_data.size());
}

TEST_F(PeerTest, SetInvalidAdvertisingData) {
  peer().MutLe().SetAdvertisingData(
      /*rssi=*/32, kInvalidAdvData, pw::chrono::SystemClock::time_point());

#ifndef NINSPECT
  EXPECT_EQ(1, InspectAdvertisingDataParseFailureCount());
  EXPECT_EQ(AdvertisingData::ParseErrorToString(
                AdvertisingData::ParseError::kUuidsMalformed),
            InspectLastAdvertisingDataParseFailure());
#endif  // NINSPECT

  EXPECT_EQ(peer().MutLe().advertising_data().size(), 0u);
}

TEST_F(PeerTest, SetExtendedAdvertisingCheckDefaultValues) {
  peer().MutLe().SetAdvertisingData(
      /*rssi=*/32, kInvalidAdvData, pw::chrono::SystemClock::time_point());
  ASSERT_TRUE(peer().le().has_value());
  EXPECT_EQ(peer().le()->advertising_sid(), hci_spec::kAdvertisingSidInvalid);
  EXPECT_EQ(peer().le()->periodic_advertising_interval(),
            hci_spec::kPeriodicAdvertisingIntervalInvalid);
}

TEST_F(PeerTest, SetExtendedAdvertisingData) {
  const uint8_t kAdvertisingSid = 0x0c;
  const uint16_t kPeriodicAdvertisingInterval = 0x1234;
  peer().MutLe().SetAdvertisingData(
      /*rssi=*/32,
      kInvalidAdvData,
      pw::chrono::SystemClock::time_point(),
      kAdvertisingSid,
      kPeriodicAdvertisingInterval);
  ASSERT_TRUE(peer().le().has_value());
  EXPECT_EQ(peer().le()->advertising_sid(), kAdvertisingSid);
  EXPECT_EQ(peer().le()->periodic_advertising_interval(),
            kPeriodicAdvertisingInterval);
}

TEST_F(PeerDeathTest, RegisterTwoBrEdrConnectionsAsserts) {
  SetUpPeer(/*address=*/kAddrBrEdr, /*connectable=*/true);
  std::optional<Peer::ConnectionToken> token_0 =
      peer().MutBrEdr().RegisterConnection();
  ASSERT_DEATH_IF_SUPPORTED(
      {
        std::optional<Peer::ConnectionToken> token_1 =
            peer().MutBrEdr().RegisterConnection();
      },
      ".*already registered.*");
}

TEST_F(PeerTest,
       RegisterAndUnregisterInitializingBrEdrConnectionLeavesPeerTemporary) {
  SetUpPeer(/*address=*/kAddrBrEdr, /*connectable=*/true);
  EXPECT_TRUE(peer().identity_known());
  std::optional<Peer::InitializingConnectionToken> token =
      peer().MutBrEdr().RegisterInitializingConnection();
  EXPECT_FALSE(peer().temporary());
  token.reset();
  EXPECT_TRUE(peer().temporary());
  EXPECT_EQ(peer().bredr()->connection_state(),
            Peer::ConnectionState::kNotConnected);
#ifndef NINSPECT
  EXPECT_EQ(
      InspectBrEdrConnectionState(),
      Peer::ConnectionStateToString(Peer::ConnectionState::kNotConnected));
#endif  // NINSPECT
}

TEST_F(PeerTest, RegisterAndUnregisterBrEdrConnectionWithoutBonding) {
  SetUpPeer(/*address=*/kAddrBrEdr, /*connectable=*/true);

  int update_expiry_count = 0;
  set_update_expiry_cb([&](const Peer&) { update_expiry_count++; });
  int notify_count = 0;
  set_notify_listeners_cb(
      [&](const Peer&, Peer::NotifyListenersChange) { notify_count++; });

  std::optional<Peer::ConnectionToken> conn_token =
      peer().MutBrEdr().RegisterConnection();
  // A notification and expiry update are sent when the peer becomes
  // non-temporary, and a second notification and update expiry are sent because
  // the initializing connection is registered.
  EXPECT_EQ(update_expiry_count, 2);
  EXPECT_EQ(notify_count, 2);
  EXPECT_FALSE(peer().temporary());
  EXPECT_EQ(peer().bredr()->connection_state(),
            Peer::ConnectionState::kConnected);
#ifndef NINSPECT
  EXPECT_EQ(InspectBrEdrConnectionState(),
            Peer::ConnectionStateToString(Peer::ConnectionState::kConnected));
#endif  // NINSPECT

  conn_token.reset();
  EXPECT_EQ(update_expiry_count, 3);
  EXPECT_EQ(notify_count, 3);
  // BR/EDR peers should become non-temporary after disconnecting if not bonded.
  EXPECT_TRUE(peer().temporary());
  EXPECT_EQ(peer().bredr()->connection_state(),
            Peer::ConnectionState::kNotConnected);
#ifndef NINSPECT
  EXPECT_EQ(
      InspectBrEdrConnectionState(),
      Peer::ConnectionStateToString(Peer::ConnectionState::kNotConnected));
#endif  // NINSPECT
}

TEST_F(PeerTest, RegisterAndUnregisterBrEdrConnectionWithBonding) {
  SetUpPeer(/*address=*/kAddrBrEdr, /*connectable=*/true);

  int update_expiry_count = 0;
  set_update_expiry_cb([&](const Peer&) { update_expiry_count++; });
  int notify_count = 0;
  set_notify_listeners_cb(
      [&](const Peer&, Peer::NotifyListenersChange) { notify_count++; });

  std::optional<Peer::ConnectionToken> conn_token =
      peer().MutBrEdr().RegisterConnection();
  // A notification and expiry update are sent when the peer becomes
  // non-temporary, and a second notification and update expiry are sent because
  // the initializing connection is registered.
  EXPECT_EQ(update_expiry_count, 2);
  EXPECT_EQ(notify_count, 2);
  EXPECT_FALSE(peer().temporary());
  EXPECT_EQ(peer().bredr()->connection_state(),
            Peer::ConnectionState::kConnected);
#ifndef NINSPECT
  EXPECT_EQ(InspectBrEdrConnectionState(),
            Peer::ConnectionStateToString(Peer::ConnectionState::kConnected));
#endif  // NINSPECT

  EXPECT_TRUE(peer().MutBrEdr().SetBondData(kSecureBrEdrKey));
  EXPECT_EQ(update_expiry_count, 2);
  EXPECT_EQ(notify_count, 3);

  conn_token.reset();
  EXPECT_EQ(update_expiry_count, 3);
  EXPECT_EQ(notify_count, 4);
  // Bonded BR/EDR peers should remain non-temporary after disconnecting.
  EXPECT_FALSE(peer().temporary());
  EXPECT_EQ(peer().bredr()->connection_state(),
            Peer::ConnectionState::kNotConnected);
#ifndef NINSPECT
  EXPECT_EQ(
      InspectBrEdrConnectionState(),
      Peer::ConnectionStateToString(Peer::ConnectionState::kNotConnected));
#endif  // NINSPECT
}

TEST_F(PeerTest,
       RegisterAndUnregisterBrEdrConnectionDuringInitializingConnection) {
  SetUpPeer(/*address=*/kAddrBrEdr, /*connectable=*/true);

  int update_expiry_count = 0;
  set_update_expiry_cb([&](const Peer&) { update_expiry_count++; });
  int notify_count = 0;
  set_notify_listeners_cb(
      [&](const Peer&, Peer::NotifyListenersChange) { notify_count++; });

  std::optional<Peer::InitializingConnectionToken> init_token =
      peer().MutBrEdr().RegisterInitializingConnection();
  // Expiry is updated for state change + becoming non-temporary.
  EXPECT_EQ(update_expiry_count, 2);
  // 1 notification for becoming non-temporary.
  EXPECT_EQ(notify_count, 1);
  EXPECT_FALSE(peer().temporary());
  EXPECT_EQ(peer().bredr()->connection_state(),
            Peer::ConnectionState::kInitializing);
#ifndef NINSPECT
  EXPECT_EQ(
      InspectBrEdrConnectionState(),
      Peer::ConnectionStateToString(Peer::ConnectionState::kInitializing));
#endif  // NINSPECT

  // The connection state should not change when registering a connection
  // because the peer is still initializing.
  std::optional<Peer::ConnectionToken> conn_token =
      peer().MutBrEdr().RegisterConnection();
  EXPECT_EQ(update_expiry_count, 2);
  EXPECT_EQ(notify_count, 1);
  EXPECT_FALSE(peer().temporary());
  EXPECT_EQ(peer().bredr()->connection_state(),
            Peer::ConnectionState::kInitializing);
#ifndef NINSPECT
  EXPECT_EQ(
      InspectBrEdrConnectionState(),
      Peer::ConnectionStateToString(Peer::ConnectionState::kInitializing));
#endif  // NINSPECT

  conn_token.reset();
  EXPECT_EQ(update_expiry_count, 2);
  EXPECT_EQ(notify_count, 1);
  EXPECT_FALSE(peer().temporary());
  EXPECT_EQ(peer().bredr()->connection_state(),
            Peer::ConnectionState::kInitializing);
#ifndef NINSPECT
  EXPECT_EQ(
      InspectBrEdrConnectionState(),
      Peer::ConnectionStateToString(Peer::ConnectionState::kInitializing));
#endif  // NINSPECT

  init_token.reset();
  EXPECT_EQ(update_expiry_count, 3);
  EXPECT_EQ(notify_count, 1);
  EXPECT_TRUE(peer().temporary());
  EXPECT_EQ(peer().bredr()->connection_state(),
            Peer::ConnectionState::kNotConnected);
#ifndef NINSPECT
  EXPECT_EQ(
      InspectBrEdrConnectionState(),
      Peer::ConnectionStateToString(Peer::ConnectionState::kNotConnected));
#endif  // NINSPECT
}

TEST_F(
    PeerTest,
    RegisterBrEdrConnectionDuringInitializingConnectionAndThenCompleteInitialization) {
  SetUpPeer(/*address=*/kAddrBrEdr, /*connectable=*/true);

  int update_expiry_count = 0;
  set_update_expiry_cb([&](const Peer&) { update_expiry_count++; });
  int notify_count = 0;
  set_notify_listeners_cb(
      [&](const Peer&, Peer::NotifyListenersChange) { notify_count++; });

  std::optional<Peer::InitializingConnectionToken> init_token =
      peer().MutBrEdr().RegisterInitializingConnection();
  // Expiry is updated for state change + becoming non-temporary.
  EXPECT_EQ(update_expiry_count, 2);
  // 1 notification for becoming non-temporary.
  EXPECT_EQ(notify_count, 1);
  EXPECT_FALSE(peer().temporary());
  EXPECT_EQ(peer().bredr()->connection_state(),
            Peer::ConnectionState::kInitializing);
#ifndef NINSPECT
  EXPECT_EQ(
      InspectBrEdrConnectionState(),
      Peer::ConnectionStateToString(Peer::ConnectionState::kInitializing));
#endif  // NINSPECT

  // The connection state should not change when registering a connection
  // because the peer is still initializing.
  std::optional<Peer::ConnectionToken> conn_token =
      peer().MutBrEdr().RegisterConnection();
  EXPECT_EQ(update_expiry_count, 2);
  EXPECT_EQ(notify_count, 1);
  EXPECT_FALSE(peer().temporary());
  EXPECT_EQ(peer().bredr()->connection_state(),
            Peer::ConnectionState::kInitializing);
#ifndef NINSPECT
  EXPECT_EQ(
      InspectBrEdrConnectionState(),
      Peer::ConnectionStateToString(Peer::ConnectionState::kInitializing));
#endif  // NINSPECT

  // When initialization completes, the connection state should become
  // kConnected.
  init_token.reset();
  EXPECT_EQ(update_expiry_count, 3);
  EXPECT_EQ(notify_count, 2);
  EXPECT_FALSE(peer().temporary());
  EXPECT_EQ(peer().bredr()->connection_state(),
            Peer::ConnectionState::kConnected);
#ifndef NINSPECT
  EXPECT_EQ(InspectBrEdrConnectionState(),
            Peer::ConnectionStateToString(Peer::ConnectionState::kConnected));
#endif  // NINSPECT

  conn_token.reset();
  EXPECT_EQ(update_expiry_count, 4);
  EXPECT_EQ(notify_count, 3);
  EXPECT_TRUE(peer().temporary());
  EXPECT_EQ(peer().bredr()->connection_state(),
            Peer::ConnectionState::kNotConnected);
#ifndef NINSPECT
  EXPECT_EQ(
      InspectBrEdrConnectionState(),
      Peer::ConnectionStateToString(Peer::ConnectionState::kNotConnected));
#endif  // NINSPECT
}

TEST_F(PeerDeathTest,
       RegisterInitializingBrEdrConnectionDuringConnectionAsserts) {
  SetUpPeer(/*address=*/kAddrBrEdr, /*connectable=*/true);

  int update_expiry_count = 0;
  set_update_expiry_cb([&](const Peer&) { update_expiry_count++; });
  int notify_count = 0;
  set_notify_listeners_cb(
      [&](const Peer&, Peer::NotifyListenersChange) { notify_count++; });

  std::optional<Peer::ConnectionToken> conn_token =
      peer().MutBrEdr().RegisterConnection();
  // A notification and expiry update are sent when the peer becomes
  // non-temporary, and a second notification and update expiry are sent because
  // the initializing connection is registered.
  EXPECT_EQ(update_expiry_count, 2);
  EXPECT_EQ(notify_count, 2);
  EXPECT_FALSE(peer().temporary());
  EXPECT_EQ(peer().bredr()->connection_state(),
            Peer::ConnectionState::kConnected);
#ifndef NINSPECT
  EXPECT_EQ(InspectBrEdrConnectionState(),
            Peer::ConnectionStateToString(Peer::ConnectionState::kConnected));
#endif  // NINSPECT

  // Registering an initializing connection when the peer is already connected
  // should assert.
  ASSERT_DEATH_IF_SUPPORTED(
      {
        Peer::InitializingConnectionToken init_token =
            peer().MutBrEdr().RegisterInitializingConnection();
      },
      ".*connected.*");
}

TEST_F(PeerTest, RegisterAndUnregisterTwoBrEdrInitializingConnections) {
  SetUpPeer(/*address=*/kAddrBrEdr, /*connectable=*/true);

  int update_expiry_count = 0;
  set_update_expiry_cb([&](const Peer&) { update_expiry_count++; });
  int notify_count = 0;
  set_notify_listeners_cb(
      [&](const Peer&, Peer::NotifyListenersChange) { notify_count++; });

  std::optional<Peer::InitializingConnectionToken> token_0 =
      peer().MutBrEdr().RegisterInitializingConnection();
  EXPECT_EQ(update_expiry_count, 2);
  EXPECT_EQ(notify_count, 1);
  EXPECT_FALSE(peer().temporary());
  EXPECT_EQ(peer().bredr()->connection_state(),
            Peer::ConnectionState::kInitializing);
#ifndef NINSPECT
  std::optional<std::string> inspect_conn_state = InspectBrEdrConnectionState();
  ASSERT_TRUE(inspect_conn_state);
  EXPECT_EQ(
      inspect_conn_state.value(),
      Peer::ConnectionStateToString(Peer::ConnectionState::kInitializing));
#endif  // NINSPECT

  std::optional<Peer::InitializingConnectionToken> token_1 =
      peer().MutBrEdr().RegisterInitializingConnection();
  // The second initializing connection should not update expiry or notify.
  EXPECT_EQ(update_expiry_count, 2);
  EXPECT_EQ(notify_count, 1);
  EXPECT_FALSE(peer().temporary());
  EXPECT_EQ(peer().bredr()->connection_state(),
            Peer::ConnectionState::kInitializing);
#ifndef NINSPECT
  inspect_conn_state = InspectBrEdrConnectionState();
  ASSERT_TRUE(inspect_conn_state);
  EXPECT_EQ(
      inspect_conn_state.value(),
      Peer::ConnectionStateToString(Peer::ConnectionState::kInitializing));
#endif  // NINSPECT

  token_0.reset();
  EXPECT_EQ(update_expiry_count, 2);
  EXPECT_EQ(notify_count, 1);
  EXPECT_FALSE(peer().temporary());
  EXPECT_EQ(peer().bredr()->connection_state(),
            Peer::ConnectionState::kInitializing);
#ifndef NINSPECT
  inspect_conn_state = InspectBrEdrConnectionState();
  ASSERT_TRUE(inspect_conn_state);
  EXPECT_EQ(
      inspect_conn_state.value(),
      Peer::ConnectionStateToString(Peer::ConnectionState::kInitializing));
#endif  // NINSPECT

  token_1.reset();
  EXPECT_EQ(update_expiry_count, 3);
  EXPECT_EQ(notify_count, 1);
  EXPECT_TRUE(peer().temporary());
  EXPECT_EQ(peer().bredr()->connection_state(),
            Peer::ConnectionState::kNotConnected);
#ifndef NINSPECT
  inspect_conn_state = InspectBrEdrConnectionState();
  ASSERT_TRUE(inspect_conn_state);
  EXPECT_EQ(
      inspect_conn_state.value(),
      Peer::ConnectionStateToString(Peer::ConnectionState::kNotConnected));
#endif  // NINSPECT
}

TEST_F(PeerTest, SettingLeAdvertisingDataOfBondedPeerDoesNotUpdateName) {
  peer().RegisterName("alice");
  sm::PairingData data;
  data.peer_ltk = kLTK;
  data.local_ltk = kLTK;
  peer().MutLe().SetBondData(data);

  const StaticByteBuffer kBondedAdvData(0x08,  // Length
                                        0x09,  // AD type: Complete Local Name
                                        'M',
                                        'a',
                                        'l',
                                        'l',
                                        'o',
                                        'r',
                                        'y');
  peer().MutLe().SetAdvertisingData(
      /*rssi=*/0,
      kBondedAdvData,
      pw::chrono::SystemClock::time_point(std::chrono::nanoseconds(0)));

  ASSERT_TRUE(peer().name().has_value());
  EXPECT_EQ(peer().name().value(), "alice");
}

TEST_F(PeerTest, SettingInquiryDataOfBondedPeerDoesNotUpdateName) {
  peer().RegisterName("alice");
  EXPECT_TRUE(peer().MutBrEdr().SetBondData(kLTK));

  const StaticByteBuffer kEirData(0x08,  // Length
                                  0x09,  // AD type: Complete Local Name
                                  'M',
                                  'a',
                                  'l',
                                  'l',
                                  'o',
                                  'r',
                                  'y');
  StaticPacket<pw::bluetooth::emboss::ExtendedInquiryResultEventWriter> eirep;
  eirep.view().num_responses().Write(1);
  eirep.view().bd_addr().CopyFrom(peer().address().value().view());
  eirep.view().extended_inquiry_response().BackingStorage().CopyFrom(
      ::emboss::support::ReadOnlyContiguousBuffer(&kEirData), kEirData.size());

  peer().MutBrEdr().SetInquiryData(eirep.view());

  ASSERT_TRUE(peer().name().has_value());
  EXPECT_EQ(peer().name().value(), "alice");
}

TEST_F(PeerTest, BrEdrDataSetEirDataDoesUpdatePeerName) {
  peer().MutBrEdr();  // Initialize BrEdrData.
  ASSERT_FALSE(peer().name().has_value());

  bool listener_notified = false;
  set_notify_listeners_cb(
      [&](auto&, Peer::NotifyListenersChange) { listener_notified = true; });

  const StaticByteBuffer kEirData(0x0D,  // Length (13)
                                  0x09,  // AD type: Complete Local Name
                                  'S',
                                  'a',
                                  'p',
                                  'p',
                                  'h',
                                  'i',
                                  'r',
                                  'e',
                                  0xf0,
                                  0x9f,
                                  0x92,
                                  0x96);

  StaticPacket<pw::bluetooth::emboss::ExtendedInquiryResultEventWriter> eirep;
  eirep.view().num_responses().Write(1);
  eirep.view().bd_addr().CopyFrom(peer().address().value().view());
  eirep.view().extended_inquiry_response().BackingStorage().CopyFrom(
      ::emboss::support::ReadOnlyContiguousBuffer(&kEirData), kEirData.size());

  peer().MutBrEdr().SetInquiryData(eirep.view());

  EXPECT_TRUE(listener_notified);  // Fresh EIR data results in an update
  ASSERT_TRUE(peer().name().has_value());
  EXPECT_EQ(peer().name().value(), "Sapphire💖");
}

TEST_F(PeerTest, SetEirDataUpdatesServiceUUIDs) {
  peer().MutBrEdr();  // Initialize BrEdrData.
                      // clang-format off
  const StaticByteBuffer kEirJustServiceUuids{
      // One 16-bit UUID: AudioSink
      0x03, static_cast<uint8_t>(DataType::kIncomplete16BitServiceUuids), 0x0A, 0x11,
  };

  StaticPacket<pw::bluetooth::emboss::ExtendedInquiryResultEventWriter> eirep;
  eirep.view().num_responses().Write(1);
  eirep.view().bd_addr().CopyFrom(peer().address().value().view());
  eirep.view().extended_inquiry_response().BackingStorage().CopyFrom(
      ::emboss::support::ReadOnlyContiguousBuffer(&kEirJustServiceUuids),
      kEirJustServiceUuids.size());
  peer().MutBrEdr().SetInquiryData(eirep.view());

  EXPECT_EQ(peer().bredr()->services().size(), 1u);
  EXPECT_EQ(peer().bredr()->services().count(UUID((uint16_t)0x110A)), 1u);
}

TEST_F(PeerTest, LowEnergyStoreBondCallsCallback) {
  int cb_count = 0;
  set_store_le_bond_cb([&cb_count](const sm::PairingData&) {
    cb_count++;
    return true;
  });

  sm::PairingData data;
  data.peer_ltk = kLTK;
  data.local_ltk = kLTK;
  EXPECT_TRUE(peer().MutLe().StoreBond(data));
  EXPECT_EQ(cb_count, 1);
}

TEST_F(PeerTest, DowngradingBrEdrBondFails) {
  EXPECT_TRUE(peer().MutBrEdr().SetBondData(kSecureBrEdrKey));
  EXPECT_FALSE(peer().MutBrEdr().SetBondData(kLessSecureBrEdrKey));
  ASSERT_TRUE(peer().MutBrEdr().link_key().has_value());
  EXPECT_EQ(peer().MutBrEdr().link_key().value(), kSecureBrEdrKey);
}

TEST_F(PeerTest, OverwritingBrEdrBondWithSameSecuritySucceeds) {
  EXPECT_TRUE(peer().MutBrEdr().SetBondData(kSecureBrEdrKey));
  EXPECT_TRUE(peer().MutBrEdr().SetBondData(kSecureBrEdrKey2));
  ASSERT_TRUE(peer().MutBrEdr().link_key().has_value());
  EXPECT_EQ(peer().MutBrEdr().link_key().value(), kSecureBrEdrKey2);
}

TEST_F(PeerTest, LowEnergyPairingToken) {
  EXPECT_FALSE(peer().MutLe().is_pairing());
  int count_0 = 0;
  peer().MutLe().add_pairing_completion_callback([&count_0](){count_0++;});
  EXPECT_EQ(count_0, 1);
  std::optional<Peer::PairingToken> token = peer().MutLe().RegisterPairing();
  int count_1 = 0;
  peer().MutLe().add_pairing_completion_callback([&count_1](){count_1++;});
  int count_2 = 0;
  peer().MutLe().add_pairing_completion_callback([&count_2](){count_2++;});
  EXPECT_EQ(count_1, 0);
  EXPECT_EQ(count_2, 0);
  token.reset();
  EXPECT_EQ(count_1, 1);
  EXPECT_EQ(count_2, 1);
}

TEST_F(PeerTest, BrEdrPairingToken) {
  EXPECT_FALSE(peer().MutBrEdr().is_pairing());
  int count_0 = 0;
  peer().MutBrEdr().add_pairing_completion_callback([&count_0]{count_0++;});
  EXPECT_EQ(count_0, 1);
  std::optional<Peer::PairingToken> token = peer().MutBrEdr().RegisterPairing();
  int count_1 = 0;
  peer().MutBrEdr().add_pairing_completion_callback([&count_1]{count_1++;});
  int count_2 = 0;
  peer().MutBrEdr().add_pairing_completion_callback([&count_2]{count_2++;});
  EXPECT_EQ(count_1, 0);
  EXPECT_EQ(count_2, 0);
  token.reset();
  EXPECT_EQ(count_1, 1);
  EXPECT_EQ(count_2, 1);
}

TEST_F(PeerTest, ClearBondDataDoesNotSetIdentityKnownToFalseIfAddressIsLEPublic) {
  ASSERT_EQ(peer().address().type(), DeviceAddress::Type::kLEPublic);
  EXPECT_TRUE(peer().identity_known());
  sm::PairingData data;
  data.peer_ltk = kLTK;
  data.local_ltk = kLTK;
  data.irk = sm::Key(sm::SecurityProperties(), UInt128{4});
  peer().MutLe().SetBondData(data);
  EXPECT_TRUE(peer().identity_known());
  peer().MutLe().ClearBondData();
  EXPECT_TRUE(peer().identity_known());
}

TEST_F(PeerTest, SetInquiryDataWithInvalidRssiIgnored) {
  EXPECT_EQ(peer().rssi(), hci_spec::kRSSIInvalid);

  const StaticByteBuffer kEirData(
      0x05,  // Length
      0x09,  // AD type: Complete Local Name
      'T',
      'e',
      's',
      't'
  );
  StaticPacket<pw::bluetooth::emboss::ExtendedInquiryResultEventWriter> eirep;
  eirep.view().num_responses().Write(1);
  eirep.view().bd_addr().CopyFrom(peer().address().value().view());
  eirep.view().rssi().UncheckedWrite(hci_spec::kMaxRssi + 1);
  eirep.view().extended_inquiry_response().BackingStorage().CopyFrom(
      ::emboss::support::ReadOnlyContiguousBuffer(&kEirData), kEirData.size());
  peer().MutBrEdr().SetInquiryData(eirep.view());
  EXPECT_EQ(peer().rssi(), hci_spec::kRSSIInvalid);

  StaticPacket<pw::bluetooth::emboss::InquiryResultWithRssiWriter> inquiry_result_rssi;
  inquiry_result_rssi.view().bd_addr().CopyFrom(peer().address().value().view());
  inquiry_result_rssi.view().rssi().UncheckedWrite(hci_spec::kMaxRssi + 1);
  peer().MutBrEdr().SetInquiryData(inquiry_result_rssi.view());
  EXPECT_EQ(peer().rssi(), hci_spec::kRSSIInvalid);
}

}  // namespace
}  // namespace bt::gap
