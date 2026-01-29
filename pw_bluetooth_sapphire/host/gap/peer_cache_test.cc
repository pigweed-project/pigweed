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

#include "pw_bluetooth_sapphire/internal/host/gap/peer_cache.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <pw_async/fake_dispatcher_fixture.h>

#include <vector>

#include "pw_bluetooth_sapphire/internal/host/common/device_class.h"
#include "pw_bluetooth_sapphire/internal/host/common/random.h"
#include "pw_bluetooth_sapphire/internal/host/common/uint128.h"
#include "pw_bluetooth_sapphire/internal/host/common/uuid.h"
#include "pw_bluetooth_sapphire/internal/host/gap/peer.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/link_key.h"
#include "pw_bluetooth_sapphire/internal/host/hci/low_energy_scanner.h"
#include "pw_bluetooth_sapphire/internal/host/sm/smp.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"
#include "pw_bluetooth_sapphire/internal/host/sm/util.h"
#include "pw_bluetooth_sapphire/internal/host/testing/inspect.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"

namespace bt::gap {
namespace {

using namespace inspect::testing;

// All fields are initialized to zero as they are unused in these tests.
const hci_spec::LEConnectionParameters kTestParams;

// Arbitrary ID value used by the bonding tests below. The actual value of this
// constant does not effect the test logic.
constexpr PeerId kId(100);
constexpr int8_t kTestRSSI = 10;

const DeviceAddress kAddrBrEdr(DeviceAddress::Type::kBREDR,
                               {0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA});
const DeviceAddress kAddrLePublic(DeviceAddress::Type::kLEPublic,
                                  {6, 5, 4, 3, 2, 1});
// LE Public Device Address that has the same value as a BR/EDR BD_ADDR, e.g. on
// a dual-mode device.
const DeviceAddress kAddrLeAlias(DeviceAddress::Type::kLEPublic,
                                 {0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA});

// TODO(armansito): Make these adhere to privacy specification.
const DeviceAddress kAddrLeRandom(DeviceAddress::Type::kLERandom,
                                  {1, 2, 3, 4, 5, 6});
const DeviceAddress kAddrLeRandom2(DeviceAddress::Type::kLERandom,
                                   {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF});
const DeviceAddress kAddrLeAnon(DeviceAddress::Type::kLEAnonymous,
                                {1, 2, 3, 4, 5, 6});

// Arbitrary name value used by the bonding tests below. The actual value of
// this constant does not effect the test logic.
const std::string kName = "TestName";

const StaticByteBuffer kAdvData(0x05,  // Length
                                0x09,  // AD type: Complete Local Name
                                'T',
                                'e',
                                's',
                                't');
const auto kEirData = kAdvData;

const bt::sm::LTK kLTK;
const bt::sm::Key kKey{};

const bt::sm::LTK kBrEdrKey;
const bt::sm::LTK kInsecureBrEdrKey(
    sm::SecurityProperties(/*encrypted=*/true,
                           /*authenticated=*/false,
                           /*secure_connections=*/false,
                           sm::kMaxEncryptionKeySize),
    hci_spec::LinkKey(UInt128{1}, 2, 3));
const bt::sm::LTK kSecureBrEdrKey(
    sm::SecurityProperties(/*encrypted=*/true,
                           /*authenticated=*/true,
                           /*secure_connections=*/true,
                           sm::kMaxEncryptionKeySize),
    hci_spec::LinkKey(UInt128{4}, 5, 6));

const std::vector<bt::UUID> kBrEdrServices = {UUID(uint16_t{0x110a}),
                                              UUID(uint16_t{0x110b})};

// Phone (Networking)
const DeviceClass kTestDeviceClass({0x06, 0x02, 0x02});

class PeerCacheTest : public pw::async::test::FakeDispatcherFixture {
 public:
  void SetUp() override { cache_ = std::make_unique<PeerCache>(dispatcher()); }

  void TearDown() override {
    RunUntilIdle();
    cache_.reset();
  }

 protected:
  // Creates a new Peer, and caches a pointer to that peer.
  [[nodiscard]] bool NewPeer(const DeviceAddress& addr, bool connectable) {
    auto* peer = cache()->NewPeer(addr, connectable);
    if (!peer) {
      return false;
    }
    peer_ = peer;
    return true;
  }

  PeerCache* cache() { return cache_.get(); }
  // Returns the cached pointer to the peer created in the most recent call to
  // NewPeer(). The caller must ensure that the peer has not expired out of
  // the cache. (Tests of cache expiration should generally subclass the
  // PeerCacheExpirationTest fixture.)
  Peer* peer() { return peer_; }

 private:
  std::unique_ptr<PeerCache> cache_;
  Peer* peer_;
};

#ifndef NINSPECT
TEST_F(PeerCacheTest, InspectHierarchyContainsMetrics) {
  inspect::Inspector inspector;
  cache()->AttachInspect(inspector.GetRoot());

  auto le_matcher = AllOf(NodeMatches(AllOf(
      NameMatches("le"),
      PropertyList(UnorderedElementsAre(UintIs("bond_success_events", 0),
                                        UintIs("bond_failure_events", 0),
                                        UintIs("connection_events", 0),
                                        UintIs("disconnection_events", 0))))));
  auto bredr_matcher = AllOf(NodeMatches(AllOf(
      NameMatches("bredr"),
      PropertyList(UnorderedElementsAre(UintIs("bond_success_events", 0),
                                        UintIs("bond_failure_events", 0),
                                        UintIs("connection_events", 0),
                                        UintIs("disconnection_events", 0))))));

  auto metrics_node_matcher =
      AllOf(NodeMatches(NameMatches(PeerMetrics::kInspectNodeName)),
            ChildrenMatch(UnorderedElementsAre(bredr_matcher, le_matcher)));

  auto peer_cache_matcher =
      AllOf(NodeMatches(AllOf(PropertyList(testing::IsEmpty()))),
            ChildrenMatch(UnorderedElementsAre(metrics_node_matcher)));

  auto hierarchy = inspect::ReadFromVmo(inspector.DuplicateVmo()).take_value();
  EXPECT_THAT(hierarchy,
              AllOf(ChildrenMatch(UnorderedElementsAre(peer_cache_matcher))));
}
#endif  // NINSPECT

#ifndef NINSPECT
TEST_F(PeerCacheTest,
       InspectHierarchyContainsAddedPeersAndDoesNotContainRemovedPeers) {
  inspect::Inspector inspector;
  cache()->AttachInspect(inspector.GetRoot());

  Peer* peer0 = cache()->NewPeer(kAddrLePublic, /*connectable=*/true);
  auto peer0_matcher = AllOf(NodeMatches(AllOf(NameMatches("peer_0x0"))));

  cache()->NewPeer(kAddrBrEdr, /*connectable=*/true);
  auto peer1_matcher = AllOf(NodeMatches(AllOf(NameMatches("peer_0x1"))));

  auto metrics_matcher =
      AllOf(NodeMatches(AllOf(NameMatches(PeerMetrics::kInspectNodeName))));

  // Hierarchy should contain peer0 and peer1.
  auto hierarchy = inspect::ReadFromVmo(inspector.DuplicateVmo()).take_value();
  auto peer_cache_matcher0 =
      AllOf(NodeMatches(AllOf(PropertyList(testing::IsEmpty()))),
            ChildrenMatch(UnorderedElementsAre(
                peer0_matcher, peer1_matcher, metrics_matcher)));
  EXPECT_THAT(hierarchy,
              AllOf(ChildrenMatch(UnorderedElementsAre(peer_cache_matcher0))));

  // peer0 should be removed from hierarchy after it is removed from the cache
  // because its Node is destroyed along with the Peer object.
  EXPECT_TRUE(cache()->RemoveDisconnectedPeer(peer0->identifier()));
  hierarchy = inspect::ReadFromVmo(inspector.DuplicateVmo()).take_value();
  auto peer_cache_matcher1 = AllOf(
      NodeMatches(AllOf(PropertyList(testing::IsEmpty()))),
      ChildrenMatch(UnorderedElementsAre(peer1_matcher, metrics_matcher)));
  EXPECT_THAT(hierarchy,
              AllOf(ChildrenMatch(UnorderedElementsAre(peer_cache_matcher1))));
}
#endif  // NINSPECT

TEST_F(PeerCacheTest, LookUp) {
  StaticByteBuffer kAdvData0(0x05, 0x09, 'T', 'e', 's', 't');
  StaticByteBuffer kAdvData1(
      0x0C, 0x09, 'T', 'e', 's', 't', ' ', 'D', 'e', 'v', 'i', 'c', 'e');

  // These should return false regardless of the input while the cache is empty.
  EXPECT_FALSE(cache()->FindByAddress(kAddrLePublic));
  EXPECT_FALSE(cache()->FindById(kId));

  auto peer = cache()->NewPeer(kAddrLePublic, /*connectable=*/true);
  ASSERT_TRUE(peer);
  ASSERT_TRUE(peer->le());
  EXPECT_EQ(TechnologyType::kLowEnergy, peer->technology());
  EXPECT_TRUE(peer->connectable());
  EXPECT_TRUE(peer->temporary());
  EXPECT_EQ(kAddrLePublic, peer->address());
  EXPECT_FALSE(peer->le()->parsed_advertising_data().has_value());
  EXPECT_EQ(hci_spec::kRSSIInvalid, peer->rssi());

  // A look up should return the same instance.
  EXPECT_EQ(peer, cache()->FindById(peer->identifier()));
  EXPECT_EQ(peer, cache()->FindByAddress(peer->address()));

  // Adding a peer with the same address should return nullptr.
  EXPECT_FALSE(cache()->NewPeer(kAddrLePublic, true));

  peer->MutLe().SetAdvertisingData(
      kTestRSSI, kAdvData1, pw::chrono::SystemClock::time_point());
  EXPECT_TRUE(peer->le()->parsed_advertising_data().has_value());
  EXPECT_EQ(kTestRSSI, peer->rssi());
  ASSERT_TRUE(peer->name().has_value());
  EXPECT_EQ(peer->name().value(), "Test Device");

  peer->MutLe().SetAdvertisingData(
      kTestRSSI, kAdvData0, pw::chrono::SystemClock::time_point());
  EXPECT_TRUE(peer->le()->parsed_advertising_data().has_value());
  EXPECT_EQ(kTestRSSI, peer->rssi());
  ASSERT_TRUE(peer->name().has_value());
  EXPECT_EQ(peer->name().value(), "Test");
}

TEST_F(PeerCacheTest, LookUpBrEdrPeerByLePublicAlias) {
  ASSERT_FALSE(cache()->FindByAddress(kAddrLeAlias));
  ASSERT_TRUE(NewPeer(kAddrBrEdr, true));
  auto* p = cache()->FindByAddress(kAddrBrEdr);
  ASSERT_TRUE(p);
  EXPECT_EQ(peer(), p);

  p = cache()->FindByAddress(kAddrLeAlias);
  ASSERT_TRUE(p);
  EXPECT_EQ(peer(), p);
  EXPECT_EQ(DeviceAddress::Type::kBREDR, p->address().type());
}

TEST_F(PeerCacheTest, LookUpLePeerByBrEdrAlias) {
  EXPECT_FALSE(cache()->FindByAddress(kAddrBrEdr));
  ASSERT_TRUE(NewPeer(kAddrLeAlias, true));
  auto* p = cache()->FindByAddress(kAddrLeAlias);
  ASSERT_TRUE(p);
  EXPECT_EQ(peer(), p);

  p = cache()->FindByAddress(kAddrBrEdr);
  ASSERT_TRUE(p);
  EXPECT_EQ(peer(), p);
  EXPECT_EQ(DeviceAddress::Type::kLEPublic, p->address().type());
}

TEST_F(PeerCacheTest, NewPeerDoesNotCrashWhenNoCallbackIsRegistered) {
  cache()->NewPeer(kAddrLePublic, /*connectable=*/true);
}

TEST_F(PeerCacheTest, ForEachEmpty) {
  bool found = false;
  cache()->ForEach([&](const auto&) { found = true; });
  EXPECT_FALSE(found);
}

TEST_F(PeerCacheTest, ForEach) {
  int count = 0;
  ASSERT_TRUE(NewPeer(kAddrLePublic, true));
  cache()->ForEach([&](const auto& p) {
    count++;
    EXPECT_EQ(peer()->identifier(), p.identifier());
    EXPECT_EQ(peer()->address(), p.address());
  });
  EXPECT_EQ(1, count);
}

TEST_F(PeerCacheTest, NewPeerInvokesCallbackWhenPeerIsFirstRegistered) {
  bool was_called = false;
  cache()->add_peer_updated_callback(
      [&was_called](const auto&) { was_called = true; });
  cache()->NewPeer(kAddrLePublic, /*connectable=*/true);
  EXPECT_TRUE(was_called);
}

TEST_F(PeerCacheTest, MultiplePeerUpdatedCallbacks) {
  size_t updated_count_0 = 0, updated_count_1 = 0;
  PeerCache::CallbackId id_0 = cache()->add_peer_updated_callback(
      [&](const auto&) { updated_count_0++; });
  PeerCache::CallbackId id_1 = cache()->add_peer_updated_callback(
      [&](const auto&) { updated_count_1++; });

  cache()->NewPeer(kAddrLePublic, /*connectable=*/true);
  EXPECT_EQ(updated_count_0, 1u);
  EXPECT_EQ(updated_count_1, 1u);

  cache()->NewPeer(kAddrLeRandom, /*connectable=*/true);
  EXPECT_EQ(updated_count_0, 2u);
  EXPECT_EQ(updated_count_1, 2u);

  EXPECT_TRUE(cache()->remove_peer_updated_callback(id_0));
  EXPECT_FALSE(cache()->remove_peer_updated_callback(id_0));
  cache()->NewPeer(kAddrLeRandom2, /*connectable=*/true);
  EXPECT_EQ(updated_count_0, 2u);
  EXPECT_EQ(updated_count_1, 3u);

  EXPECT_TRUE(cache()->remove_peer_updated_callback(id_1));
  EXPECT_FALSE(cache()->remove_peer_updated_callback(id_1));
  cache()->NewPeer(kAddrBrEdr, /*connectable=*/true);
  EXPECT_EQ(updated_count_0, 2u);
  EXPECT_EQ(updated_count_1, 3u);
}

TEST_F(PeerCacheTest, NewPeerDoesNotInvokeCallbackWhenPeerIsReRegistered) {
  int call_count = 0;
  cache()->add_peer_updated_callback(
      [&call_count](const auto&) { ++call_count; });
  cache()->NewPeer(kAddrLePublic, /*connectable=*/true);
  cache()->NewPeer(kAddrLePublic, /*connectable=*/true);
  EXPECT_EQ(1, call_count);
}

TEST_F(PeerCacheTest, NewPeerIdentityKnown) {
  EXPECT_TRUE(cache()->NewPeer(kAddrBrEdr, true)->identity_known());
  EXPECT_TRUE(cache()->NewPeer(kAddrLePublic, true)->identity_known());
  EXPECT_FALSE(cache()->NewPeer(kAddrLeRandom, true)->identity_known());
  EXPECT_FALSE(cache()->NewPeer(kAddrLeAnon, false)->identity_known());
}

TEST_F(PeerCacheTest, NewPeerInitialTechnologyIsClassic) {
  ASSERT_TRUE(NewPeer(kAddrBrEdr, true));

  // A peer initialized with a BR/EDR address should start out as a
  // classic-only.
  ASSERT_TRUE(peer());
  EXPECT_TRUE(peer()->bredr());
  EXPECT_FALSE(peer()->le());
  EXPECT_TRUE(peer()->identity_known());
  EXPECT_EQ(TechnologyType::kClassic, peer()->technology());
}

TEST_F(PeerCacheTest, NewPeerInitialTechnologyLowEnergy) {
  // LE address types should initialize the peer as LE-only.
  auto* le_publ_peer = cache()->NewPeer(kAddrLePublic, /*connectable=*/true);
  auto* le_rand_peer = cache()->NewPeer(kAddrLeRandom, /*connectable=*/true);
  auto* le_anon_peer = cache()->NewPeer(kAddrLeAnon, /*connectable=*/false);
  ASSERT_TRUE(le_publ_peer);
  ASSERT_TRUE(le_rand_peer);
  ASSERT_TRUE(le_anon_peer);
  EXPECT_TRUE(le_publ_peer->le());
  EXPECT_TRUE(le_rand_peer->le());
  EXPECT_TRUE(le_anon_peer->le());
  EXPECT_FALSE(le_publ_peer->bredr());
  EXPECT_FALSE(le_rand_peer->bredr());
  EXPECT_FALSE(le_anon_peer->bredr());
  EXPECT_EQ(TechnologyType::kLowEnergy, le_publ_peer->technology());
  EXPECT_EQ(TechnologyType::kLowEnergy, le_rand_peer->technology());
  EXPECT_EQ(TechnologyType::kLowEnergy, le_anon_peer->technology());
  EXPECT_TRUE(le_publ_peer->identity_known());
  EXPECT_FALSE(le_rand_peer->identity_known());
  EXPECT_FALSE(le_anon_peer->identity_known());
}

TEST_F(PeerCacheTest, DisallowNewLowEnergyPeerIfBrEdrPeerExists) {
  ASSERT_TRUE(NewPeer(kAddrBrEdr, true));

  // Try to add new LE peer with a public identity address containing the same
  // value as the existing BR/EDR peer's BD_ADDR.
  auto* le_alias_peer = cache()->NewPeer(kAddrLeAlias, /*connectable=*/true);
  EXPECT_FALSE(le_alias_peer);
}

TEST_F(PeerCacheTest, DisallowNewBrEdrPeerIfLowEnergyPeerExists) {
  ASSERT_TRUE(NewPeer(kAddrLeAlias, true));

  // Try to add new BR/EDR peer with BD_ADDR containing the same value as the
  // existing LE peer's public identity address.
  auto* bredr_alias_peer = cache()->NewPeer(kAddrBrEdr, /*connectable=*/true);
  ASSERT_FALSE(bredr_alias_peer);
}

TEST_F(PeerCacheTest, BrEdrPeerBecomesDualModeWithAdvertisingData) {
  ASSERT_TRUE(NewPeer(kAddrBrEdr, true));
  ASSERT_TRUE(peer()->bredr());
  ASSERT_FALSE(peer()->le());

  peer()->MutLe().SetAdvertisingData(
      kTestRSSI, kAdvData, pw::chrono::SystemClock::time_point());
  EXPECT_TRUE(peer()->le());
  EXPECT_EQ(TechnologyType::kDualMode, peer()->technology());

  // Searching by LE address should turn up this peer, which should retain its
  // original address type.
  auto* const le_peer = cache()->FindByAddress(kAddrLeAlias);
  ASSERT_EQ(peer(), le_peer);
  EXPECT_EQ(DeviceAddress::Type::kBREDR, peer()->address().type());
}

TEST_F(PeerCacheTest, BrEdrPeerBecomesDualModeWhenConnectedOverLowEnergy) {
  ASSERT_TRUE(NewPeer(kAddrBrEdr, true));
  ASSERT_TRUE(peer()->bredr());
  ASSERT_FALSE(peer()->le());

  Peer::ConnectionToken conn_token = peer()->MutLe().RegisterConnection();
  EXPECT_TRUE(peer()->le());
  EXPECT_EQ(TechnologyType::kDualMode, peer()->technology());

  auto* const le_peer = cache()->FindByAddress(kAddrLeAlias);
  ASSERT_EQ(peer(), le_peer);
  EXPECT_EQ(DeviceAddress::Type::kBREDR, peer()->address().type());
}

TEST_F(PeerCacheTest, BrEdrPeerBecomesDualModeWithLowEnergyConnParams) {
  ASSERT_TRUE(NewPeer(kAddrBrEdr, true));
  ASSERT_TRUE(peer()->bredr());
  ASSERT_FALSE(peer()->le());

  peer()->MutLe().SetConnectionParameters({});
  EXPECT_TRUE(peer()->le());
  EXPECT_EQ(TechnologyType::kDualMode, peer()->technology());

  auto* const le_peer = cache()->FindByAddress(kAddrLeAlias);
  ASSERT_EQ(peer(), le_peer);
  EXPECT_EQ(DeviceAddress::Type::kBREDR, peer()->address().type());
}

TEST_F(PeerCacheTest,
       BrEdrPeerBecomesDualModeWithLowEnergyPreferredConnParams) {
  ASSERT_TRUE(NewPeer(kAddrBrEdr, true));
  ASSERT_TRUE(peer()->bredr());
  ASSERT_FALSE(peer()->le());

  peer()->MutLe().SetPreferredConnectionParameters({});
  EXPECT_TRUE(peer()->le());
  EXPECT_EQ(TechnologyType::kDualMode, peer()->technology());

  auto* const le_peer = cache()->FindByAddress(kAddrLeAlias);
  ASSERT_EQ(peer(), le_peer);
  EXPECT_EQ(DeviceAddress::Type::kBREDR, peer()->address().type());
}

TEST_F(PeerCacheTest, LowEnergyPeerBecomesDualModeWithInquiryData) {
  ASSERT_TRUE(NewPeer(kAddrLeAlias, true));
  ASSERT_TRUE(peer()->le());
  ASSERT_FALSE(peer()->bredr());

  StaticPacket<pw::bluetooth::emboss::InquiryResultWriter> ir;
  ir.view().bd_addr().CopyFrom(kAddrLeAlias.value().view());
  peer()->MutBrEdr().SetInquiryData(ir.view());
  EXPECT_TRUE(peer()->bredr());
  EXPECT_EQ(TechnologyType::kDualMode, peer()->technology());

  // Searching by only BR/EDR technology should turn up this peer, which
  // should still retain its original address type.
  auto* const bredr_peer = cache()->FindByAddress(kAddrBrEdr);
  ASSERT_EQ(peer(), bredr_peer);
  EXPECT_EQ(DeviceAddress::Type::kLEPublic, peer()->address().type());
  EXPECT_EQ(kAddrBrEdr, peer()->bredr()->address());
}

TEST_F(PeerCacheTest, LowEnergyPeerBecomesDualModeWhenConnectedOverClassic) {
  ASSERT_TRUE(NewPeer(kAddrLeAlias, true));
  ASSERT_TRUE(peer()->le());
  ASSERT_FALSE(peer()->bredr());

  Peer::ConnectionToken token = peer()->MutBrEdr().RegisterConnection();
  EXPECT_TRUE(peer()->bredr());
  EXPECT_EQ(TechnologyType::kDualMode, peer()->technology());

  auto* const bredr_peer = cache()->FindByAddress(kAddrBrEdr);
  ASSERT_EQ(peer(), bredr_peer);
  EXPECT_EQ(DeviceAddress::Type::kLEPublic, peer()->address().type());
  EXPECT_EQ(kAddrBrEdr, peer()->bredr()->address());
}

TEST_F(PeerCacheTest, InitialAutoConnectBehavior) {
  ASSERT_TRUE(NewPeer(kAddrLeAlias, true));

  // Peers are not autoconnected before they are bonded.
  EXPECT_FALSE(peer()->le()->should_auto_connect());

  sm::PairingData data;
  data.peer_ltk = sm::LTK();
  data.local_ltk = sm::LTK();
  EXPECT_TRUE(cache()->StoreLowEnergyBond(peer()->identifier(), data));

  // Bonded peers should autoconnect
  EXPECT_TRUE(peer()->le()->should_auto_connect());

  // Connecting peer leaves `should_auto_connect` unaffected.
  Peer::ConnectionToken conn_token = peer()->MutLe().RegisterConnection();

  EXPECT_TRUE(peer()->le()->should_auto_connect());
}

TEST_F(PeerCacheTest, AutoConnectDisabledAfterIntentionalDisconnect) {
  ASSERT_TRUE(NewPeer(kAddrLeAlias, true));
  cache()->SetAutoConnectBehaviorForIntentionalDisconnect(peer()->identifier());
  EXPECT_FALSE(peer()->le()->should_auto_connect());
}

TEST_F(PeerCacheTest, AutoConnectReenabledAfterSuccessfulConnect) {
  ASSERT_TRUE(NewPeer(kAddrLeAlias, true));

  // Only bonded peers are eligible for autoconnect.
  sm::PairingData data;
  data.peer_ltk = sm::LTK();
  data.local_ltk = sm::LTK();
  EXPECT_TRUE(cache()->StoreLowEnergyBond(peer()->identifier(), data));

  cache()->SetAutoConnectBehaviorForIntentionalDisconnect(peer()->identifier());
  EXPECT_FALSE(peer()->le()->should_auto_connect());

  cache()->SetAutoConnectBehaviorForSuccessfulConnection(peer()->identifier());
  EXPECT_TRUE(peer()->le()->should_auto_connect());
}

class PeerCacheTestBondingTest : public PeerCacheTest {
 public:
  void SetUp() override {
    PeerCacheTest::SetUp();
    ASSERT_TRUE(NewPeer(kAddrLePublic, true));
    bonded_callback_count_ = 0;
    cache()->set_peer_bonded_callback(
        [this](const auto&) { bonded_callback_count_++; });
    updated_callback_count_ = 0;
    updated_callback_id_ = cache()->add_peer_updated_callback(
        [this](auto&) { updated_callback_count_++; });
    removed_callback_count_ = 0;
    cache()->set_peer_removed_callback(
        [this](PeerId) { removed_callback_count_++; });
  }

  void TearDown() override {
    cache()->set_peer_removed_callback(nullptr);
    removed_callback_count_ = 0;
    EXPECT_TRUE(cache()->remove_peer_updated_callback(updated_callback_id_));
    updated_callback_count_ = 0;
    cache()->set_peer_bonded_callback(nullptr);
    bonded_callback_count_ = 0;
    PeerCacheTest::TearDown();
  }

 protected:
  bool bonded_callback_called() const { return bonded_callback_count_ != 0; }

  // Returns 0 at the beginning of each test case.
  int bonded_callback_count() const { return bonded_callback_count_; }

  int updated_callback_count() const { return updated_callback_count_; }

  int removed_callback_count() const { return removed_callback_count_; }

 private:
  int bonded_callback_count_;
  int updated_callback_count_;
  int removed_callback_count_;
  PeerCache::CallbackId updated_callback_id_ = 0;
};

TEST_F(PeerCacheTestBondingTest, AddBondedPeerFailsWithExistingId) {
  sm::PairingData data;
  data.peer_ltk = kLTK;
  data.local_ltk = kLTK;
  EXPECT_FALSE(
      cache()->AddBondedPeer(BondingData{.identifier = peer()->identifier(),
                                         .address = kAddrLeRandom,
                                         .name = {},
                                         .device_class = {},
                                         .le_pairing_data = data,
                                         .bredr_link_key = {},
                                         .bredr_services = {}}));
  EXPECT_FALSE(bonded_callback_called());
}

TEST_F(PeerCacheTestBondingTest, AddBondedPeerFailsWithExistingAddress) {
  sm::PairingData data;
  data.peer_ltk = kLTK;
  data.local_ltk = kLTK;
  EXPECT_FALSE(cache()->AddBondedPeer(BondingData{.identifier = kId,
                                                  .address = peer()->address(),
                                                  .name = {},
                                                  .device_class = {},
                                                  .le_pairing_data = data,
                                                  .bredr_link_key = {},
                                                  .bredr_services = {}}));
  EXPECT_FALSE(bonded_callback_called());
}

TEST_F(PeerCacheTestBondingTest,
       AddBondedLowEnergyPeerFailsWithExistingBrEdrAliasAddress) {
  EXPECT_TRUE(NewPeer(kAddrBrEdr, true));
  sm::PairingData data;
  data.peer_ltk = kLTK;
  data.local_ltk = kLTK;
  EXPECT_FALSE(cache()->AddBondedPeer(BondingData{.identifier = kId,
                                                  .address = kAddrLeAlias,
                                                  .name = {},
                                                  .device_class = {},
                                                  .le_pairing_data = data,
                                                  .bredr_link_key = {},
                                                  .bredr_services = {}}));
  EXPECT_FALSE(bonded_callback_called());
}

TEST_F(PeerCacheTestBondingTest,
       AddBondedBrEdrPeerFailsWithExistingLowEnergyAliasAddress) {
  EXPECT_TRUE(NewPeer(kAddrLeAlias, true));
  EXPECT_FALSE(cache()->AddBondedPeer(BondingData{.identifier = kId,
                                                  .address = kAddrBrEdr,
                                                  .name = {},
                                                  .device_class = {},
                                                  .le_pairing_data = {},
                                                  .bredr_link_key = kBrEdrKey,
                                                  .bredr_services = {}}));
  EXPECT_FALSE(bonded_callback_called());
}

TEST_F(PeerCacheTestBondingTest, AddBondedPeerFailsWithoutMandatoryKeys) {
  sm::PairingData data;
  EXPECT_FALSE(cache()->AddBondedPeer(BondingData{.identifier = kId,
                                                  .address = kAddrLeAlias,
                                                  .name = {},
                                                  .device_class = {},
                                                  .le_pairing_data = data,
                                                  .bredr_link_key = kBrEdrKey,
                                                  .bredr_services = {}}));
  data.peer_ltk = kLTK;
  data.local_ltk = kLTK;
  EXPECT_FALSE(cache()->AddBondedPeer(BondingData{.identifier = kId,
                                                  .address = kAddrBrEdr,
                                                  .name = {},
                                                  .device_class = {},
                                                  .le_pairing_data = data,
                                                  .bredr_link_key = {},
                                                  .bredr_services = {}}));
  EXPECT_FALSE(bonded_callback_called());
}

TEST_F(PeerCacheTestBondingTest, AddLowEnergyBondedPeerSuccess) {
  sm::PairingData data;
  data.peer_ltk = kLTK;
  data.local_ltk = kLTK;

  EXPECT_TRUE(cache()->AddBondedPeer(BondingData{.identifier = kId,
                                                 .address = kAddrLeRandom,
                                                 .name = kName,
                                                 .device_class = {},
                                                 .le_pairing_data = data,
                                                 .bredr_link_key = {},
                                                 .bredr_services = {}}));
  auto* peer = cache()->FindById(kId);
  ASSERT_TRUE(peer);
  EXPECT_EQ(peer, cache()->FindByAddress(kAddrLeRandom));
  EXPECT_EQ(kId, peer->identifier());
  EXPECT_EQ(kAddrLeRandom, peer->address());
  EXPECT_EQ(kName, peer->name());
  EXPECT_EQ(Peer::NameSource::kUnknown, peer->name_source());
  EXPECT_TRUE(peer->identity_known());
  ASSERT_TRUE(peer->le());
  EXPECT_TRUE(peer->le()->bonded());
  ASSERT_TRUE(peer->le()->bond_data());
  EXPECT_EQ(data, *peer->le()->bond_data());
  EXPECT_FALSE(peer->bredr());
  EXPECT_EQ(TechnologyType::kLowEnergy, peer->technology());

  // The "new bond" callback should not be called when restoring a previously
  // bonded peer.
  EXPECT_FALSE(bonded_callback_called());
}

TEST_F(PeerCacheTestBondingTest, AddBrEdrBondedPeerSuccess) {
  sm::PairingData data;

  EXPECT_TRUE(
      cache()->AddBondedPeer(BondingData{.identifier = kId,
                                         .address = kAddrBrEdr,
                                         .name = {},
                                         .device_class = {},
                                         .le_pairing_data = data,
                                         .bredr_link_key = kBrEdrKey,
                                         .bredr_services = kBrEdrServices}));
  auto* peer = cache()->FindById(kId);
  ASSERT_TRUE(peer);
  EXPECT_EQ(peer, cache()->FindByAddress(kAddrBrEdr));
  EXPECT_EQ(kId, peer->identifier());
  EXPECT_EQ(kAddrBrEdr, peer->address());
  ASSERT_FALSE(peer->name());
  EXPECT_TRUE(peer->identity_known());
  ASSERT_TRUE(peer->bredr());
  EXPECT_TRUE(peer->bredr()->bonded());
  ASSERT_TRUE(peer->bredr()->link_key());
  EXPECT_EQ(kBrEdrKey, *peer->bredr()->link_key());
  EXPECT_THAT(peer->bredr()->services(),
              testing::UnorderedElementsAreArray(kBrEdrServices));
  EXPECT_FALSE(peer->le());
  EXPECT_EQ(TechnologyType::kClassic, peer->technology());

  // The "new bond" callback should not be called when restoring a previously
  // bonded peer.
  EXPECT_FALSE(bonded_callback_called());
}

TEST_F(PeerCacheTest, AddBondedPeerWithIrkIsAddedToResolvingList) {
  sm::PairingData data;
  data.peer_ltk = kLTK;
  data.local_ltk = kLTK;
  data.irk = sm::Key(sm::SecurityProperties(), Random<UInt128>());
  data.identity_address = kAddrLeRandom;

  EXPECT_TRUE(cache()->AddBondedPeer(BondingData{.identifier = kId,
                                                 .address = kAddrLeRandom,
                                                 .name = {},
                                                 .device_class = {},
                                                 .le_pairing_data = data,
                                                 .bredr_link_key = {},
                                                 .bredr_services = {}}));
  auto* peer = cache()->FindByAddress(kAddrLeRandom);
  ASSERT_TRUE(peer);
  EXPECT_EQ(kAddrLeRandom, peer->address());

  // Looking up the peer by RPA generated using the IRK should return the same
  // peer.
  DeviceAddress rpa = sm::util::GenerateRpa(data.irk->value());
  EXPECT_EQ(peer, cache()->FindByAddress(rpa));
}

TEST_F(PeerCacheTest, AddBondedPeerWithIrkButWithoutIdentityAddressPanics) {
  sm::PairingData data;
  data.peer_ltk = kLTK;
  data.local_ltk = kLTK;
  data.irk = sm::Key(sm::SecurityProperties(), Random<UInt128>());

  EXPECT_DEATH_IF_SUPPORTED(
      cache()->AddBondedPeer(BondingData{.identifier = kId,
                                         .address = kAddrLeRandom,
                                         .name = {},
                                         .device_class = {},
                                         .le_pairing_data = data,
                                         .bredr_link_key = {},
                                         .bredr_services = {}}),
      ".*identity_address.*");
}

TEST_F(PeerCacheTest,
       StoreLowEnergyBondWithIrkButWithoutIdentityAddressPanics) {
  sm::PairingData data;
  data.peer_ltk = kLTK;
  data.local_ltk = kLTK;
  data.irk = sm::Key(sm::SecurityProperties(), Random<UInt128>());

  EXPECT_DEATH_IF_SUPPORTED(cache()->StoreLowEnergyBond(kId, data),
                            ".*identity_address.*");
}

TEST_F(PeerCacheTestBondingTest, StoreLowEnergyBondFailsWithNoKeys) {
  sm::PairingData data;
  EXPECT_FALSE(cache()->StoreLowEnergyBond(peer()->identifier(), data));
}

TEST_F(PeerCacheTestBondingTest, StoreLowEnergyBondPeerUnknown) {
  sm::PairingData data;
  data.peer_ltk = kLTK;
  data.local_ltk = kLTK;
  EXPECT_FALSE(cache()->StoreLowEnergyBond(kId, data));
}

TEST_F(PeerCacheTestBondingTest, StoreLowEnergyBondWithLtk) {
  ASSERT_TRUE(peer()->temporary());
  ASSERT_TRUE(peer()->le());
  ASSERT_FALSE(peer()->le()->bonded());

  sm::PairingData data;
  data.peer_ltk = kLTK;
  data.local_ltk = kLTK;
  EXPECT_TRUE(cache()->StoreLowEnergyBond(peer()->identifier(), data));

  EXPECT_TRUE(bonded_callback_called());
  EXPECT_FALSE(peer()->temporary());
  EXPECT_TRUE(peer()->le()->bonded());
  EXPECT_TRUE(peer()->le()->bond_data());
  EXPECT_EQ(data, *peer()->le()->bond_data());
}

TEST_F(PeerCacheTestBondingTest, StoreLowEnergyBondWithCsrk) {
  ASSERT_TRUE(peer()->temporary());
  ASSERT_TRUE(peer()->le());
  ASSERT_FALSE(peer()->le()->bonded());

  sm::PairingData data;
  data.csrk = kKey;
  EXPECT_TRUE(cache()->StoreLowEnergyBond(peer()->identifier(), data));

  EXPECT_TRUE(bonded_callback_called());
  EXPECT_FALSE(peer()->temporary());
  EXPECT_TRUE(peer()->le()->bonded());
  EXPECT_TRUE(peer()->le()->bond_data());
  EXPECT_EQ(data, *peer()->le()->bond_data());
}

// StoreLowEnergyBond fails if it contains the address of a different,
// previously known peer.
TEST_F(PeerCacheTestBondingTest,
       StoreLowEnergyBondWithExistingDifferentIdentity) {
  auto* p = cache()->NewPeer(kAddrLeRandom, /*connectable=*/true);

  // Assign the other peer's address as identity.
  sm::PairingData data;
  data.peer_ltk = kLTK;
  data.local_ltk = kLTK;
  data.irk = sm::Key(sm::SecurityProperties(), Random<UInt128>());
  data.identity_address = peer()->address();
  EXPECT_FALSE(cache()->StoreLowEnergyBond(p->identifier(), data));
  EXPECT_FALSE(p->le()->bonded());
  EXPECT_TRUE(p->temporary());
}

// StoreLowEnergyBond fails if the new identity is the address of a "different"
// (another peer record with a distinct ID) BR/EDR peer.
TEST_F(PeerCacheTestBondingTest,
       StoreLowEnergyBondWithNewIdentityMatchingExistingBrEdrPeer) {
  ASSERT_TRUE(NewPeer(kAddrBrEdr, true));
  ASSERT_TRUE(NewPeer(kAddrLeRandom, true));
  ASSERT_FALSE(peer()->identity_known());

  sm::PairingData data;
  data.peer_ltk = kLTK;
  data.local_ltk = kLTK;
  data.irk = sm::Key(sm::SecurityProperties(), Random<UInt128>());
  // new identity address is same as another peer's BR/EDR identity
  data.identity_address = kAddrLeAlias;
  const auto old_address = peer()->address();
  ASSERT_EQ(peer(), cache()->FindByAddress(old_address));
  ASSERT_NE(peer(), cache()->FindByAddress(*data.identity_address));
  EXPECT_FALSE(cache()->StoreLowEnergyBond(peer()->identifier(), data));
  EXPECT_FALSE(peer()->identity_known());
}

// StoreLowEnergyBond succeeds if it contains an identity address that already
// matches the target peer.
TEST_F(PeerCacheTestBondingTest,
       StoreLowEnergyBondWithExistingMatchingIdentity) {
  sm::PairingData data;
  data.peer_ltk = kLTK;
  data.local_ltk = kLTK;
  data.irk = sm::Key(sm::SecurityProperties(), Random<UInt128>());
  data.identity_address = peer()->address();
  EXPECT_TRUE(cache()->StoreLowEnergyBond(peer()->identifier(), data));
  EXPECT_TRUE(peer()->le()->bonded());
  EXPECT_EQ(peer(), cache()->FindByAddress(*data.identity_address));
}

TEST_F(PeerCacheTestBondingTest, StoreLowEnergyBondWithNewIdentity) {
  ASSERT_TRUE(NewPeer(kAddrLeRandom, true));
  ASSERT_FALSE(peer()->identity_known());

  sm::PairingData data;
  data.peer_ltk = kLTK;
  data.local_ltk = kLTK;
  data.irk = sm::Key(sm::SecurityProperties(), Random<UInt128>());
  data.identity_address = kAddrLeRandom2;  // assign a new identity address
  const auto old_address = peer()->address();
  ASSERT_EQ(peer(), cache()->FindByAddress(old_address));
  ASSERT_EQ(nullptr, cache()->FindByAddress(*data.identity_address));

  EXPECT_TRUE(cache()->StoreLowEnergyBond(peer()->identifier(), data));
  EXPECT_TRUE(peer()->le()->bonded());

  // Address should have been updated.
  ASSERT_NE(*data.identity_address, old_address);
  EXPECT_EQ(*data.identity_address, peer()->address());
  EXPECT_TRUE(peer()->identity_known());
  EXPECT_EQ(peer(), cache()->FindByAddress(*data.identity_address));

  // The old address should still map to |peer|.
  ASSERT_EQ(peer(), cache()->FindByAddress(old_address));
}

TEST_F(PeerCacheTestBondingTest,
       StoreLowEnergyBondWithIrkIsAddedToResolvingList) {
  ASSERT_TRUE(NewPeer(kAddrLeRandom, true));
  ASSERT_FALSE(peer()->identity_known());

  sm::PairingData data;
  data.peer_ltk = kLTK;
  data.local_ltk = kLTK;
  data.identity_address = kAddrLeRandom;
  data.irk = sm::Key(sm::SecurityProperties(), Random<UInt128>());

  EXPECT_TRUE(cache()->StoreLowEnergyBond(peer()->identifier(), data));
  ASSERT_TRUE(peer()->le()->bonded());
  ASSERT_TRUE(peer()->identity_known());

  // Looking up the peer by RPA generated using the IRK should return the same
  // peer.
  DeviceAddress rpa = sm::util::GenerateRpa(data.irk->value());
  EXPECT_EQ(peer(), cache()->FindByAddress(rpa));
}

TEST_F(PeerCacheTestBondingTest, RemovingPeerRemovesIrkFromResolvingList) {
  sm::PairingData data;
  data.peer_ltk = kLTK;
  data.local_ltk = kLTK;
  data.identity_address = kAddrLePublic;
  data.irk = sm::Key(sm::SecurityProperties(), Random<UInt128>());

  EXPECT_TRUE(cache()->StoreLowEnergyBond(peer()->identifier(), data));

  // Removing peer should remove IRK from resolving list, allowing a new peer to
  // be created with an RPA corresponding to the removed IRK. Because the
  // resolving list is empty, FindByAddress should look up the peer by the RPA
  // address, not the resolved address, and return the new peer.
  EXPECT_TRUE(cache()->RemoveDisconnectedPeer(peer()->identifier()));
  DeviceAddress rpa = sm::util::GenerateRpa(data.irk->value());
  EXPECT_EQ(nullptr, cache()->FindByAddress(rpa));
  ASSERT_TRUE(NewPeer(rpa, true));
  EXPECT_EQ(peer(), cache()->FindByAddress(rpa));
  // Subsequent calls to create a peer with the same RPA should fail.
  EXPECT_FALSE(NewPeer(rpa, true));
}

TEST_F(PeerCacheTestBondingTest, StoreLowEnergyBondWithXTransportKeyNoBrEdr) {
  // There's no preexisting BR/EDR data, the LE peer already exists.
  sm::PairingData data;
  data.peer_ltk = kLTK;
  data.local_ltk = kLTK;
  data.cross_transport_key = kSecureBrEdrKey;

  EXPECT_TRUE(cache()->StoreLowEnergyBond(peer()->identifier(), data));
  EXPECT_TRUE(peer()->le()->bonded());
  // Storing an LE bond with a cross-transport BR/EDR key should mark the peer
  // as dual-mode.
  EXPECT_TRUE(peer()->bredr().has_value());
  EXPECT_TRUE(peer()->bredr()->bonded());
  EXPECT_EQ(kSecureBrEdrKey, peer()->bredr()->link_key().value());
  EXPECT_EQ(bonded_callback_count(), 1);
}

TEST_F(PeerCacheTestBondingTest,
       StoreLowEnergyBondWithInsecureXTransportKeyExistingBrEdr) {
  // The peer is already dual-mode with a secure BR/EDR key.
  EXPECT_TRUE(peer()->MutBrEdr().SetBondData(kSecureBrEdrKey));
  EXPECT_TRUE(peer()->bredr()->bonded());

  sm::PairingData data;
  data.peer_ltk = kLTK;
  data.local_ltk = kLTK;
  data.cross_transport_key = kInsecureBrEdrKey;
  EXPECT_TRUE(cache()->StoreLowEnergyBond(peer()->identifier(), data));

  // Verify that the existing BR/EDR key is not overwritten by a key of lesser
  // security
  sm::LTK current_bredr_key = peer()->bredr()->link_key().value();
  EXPECT_NE(kInsecureBrEdrKey, current_bredr_key);
  EXPECT_EQ(kSecureBrEdrKey, current_bredr_key);
}

TEST_F(PeerCacheTestBondingTest,
       StoreLowEnergyBondWithSameOrMoreSecureXTransportKeyExistingBrEdr) {
  // The peer is already dual-mode with an insecure BR/EDR key.
  EXPECT_TRUE(peer()->MutBrEdr().SetBondData(kInsecureBrEdrKey));
  EXPECT_TRUE(peer()->bredr()->bonded());

  sm::LTK kDifferentInsecureBrEdrKey(kInsecureBrEdrKey.security(),
                                     hci_spec::LinkKey(UInt128{8}, 9, 10));
  sm::PairingData data;
  data.peer_ltk = kLTK;
  data.local_ltk = kLTK;
  data.cross_transport_key = kDifferentInsecureBrEdrKey;
  EXPECT_TRUE(cache()->StoreLowEnergyBond(peer()->identifier(), data));

  // Verify that the existing BR/EDR key is overwritten by a key of the same
  // security ("if the key
  // [...] already exists, then the devices shall not overwrite that existing
  // key with a key that is weaker" v5.2 Vol. 3 Part C 14.1).
  sm::LTK current_bredr_key = peer()->bredr()->link_key().value();
  EXPECT_NE(kInsecureBrEdrKey, current_bredr_key);
  EXPECT_EQ(kDifferentInsecureBrEdrKey, current_bredr_key);

  // Verify that the existing BR/EDR key is also overwritten by a key of greater
  // security.
  data.cross_transport_key = kSecureBrEdrKey;
  EXPECT_TRUE(cache()->StoreLowEnergyBond(peer()->identifier(), data));

  current_bredr_key = peer()->bredr()->link_key().value();
  EXPECT_NE(kDifferentInsecureBrEdrKey, current_bredr_key);
  EXPECT_EQ(kSecureBrEdrKey, current_bredr_key);
}

TEST_F(PeerCacheTestBondingTest, StoreBrEdrBondWithUnknownAddress) {
  ASSERT_EQ(nullptr, cache()->FindByAddress(kAddrBrEdr));
  EXPECT_FALSE(cache()->StoreBrEdrBond(kAddrBrEdr, kBrEdrKey));
}

TEST_F(PeerCacheTestBondingTest, StoreBrEdrBond) {
  ASSERT_TRUE(NewPeer(kAddrBrEdr, true));
  ASSERT_EQ(peer(), cache()->FindByAddress(kAddrBrEdr));
  ASSERT_TRUE(peer()->temporary());
  ASSERT_FALSE(peer()->bonded());
  ASSERT_TRUE(peer()->bredr());
  ASSERT_FALSE(peer()->bredr()->bonded());

  EXPECT_TRUE(cache()->StoreBrEdrBond(kAddrBrEdr, kBrEdrKey));

  EXPECT_FALSE(peer()->temporary());
  EXPECT_TRUE(peer()->bonded());
  EXPECT_TRUE(peer()->bredr()->bonded());
  EXPECT_TRUE(peer()->bredr()->link_key());
  EXPECT_EQ(kBrEdrKey, *peer()->bredr()->link_key());
}

TEST_F(PeerCacheTestBondingTest, StoreBondsForBothTech) {
  ASSERT_TRUE(NewPeer(kAddrBrEdr, true));
  ASSERT_EQ(peer(), cache()->FindByAddress(kAddrBrEdr));
  ASSERT_TRUE(peer()->temporary());
  ASSERT_FALSE(peer()->bonded());

  peer()->MutLe().SetAdvertisingData(
      kTestRSSI, kAdvData, pw::chrono::SystemClock::time_point());
  ASSERT_EQ(TechnologyType::kDualMode, peer()->technology());

  // Without Secure Connections cross-transport key generation, bonding on one
  // technology does not bond on the other.
  ASSERT_FALSE(kBrEdrKey.security().secure_connections());
  EXPECT_TRUE(cache()->StoreBrEdrBond(kAddrBrEdr, kBrEdrKey));
  EXPECT_TRUE(peer()->bonded());
  EXPECT_FALSE(peer()->le()->bonded());

  sm::PairingData data;
  data.peer_ltk = kLTK;
  data.local_ltk = kLTK;
  EXPECT_TRUE(cache()->StoreLowEnergyBond(peer()->identifier(), data));

  EXPECT_FALSE(peer()->temporary());
  EXPECT_TRUE(peer()->bonded());
  EXPECT_TRUE(peer()->bredr()->bonded());
  EXPECT_TRUE(peer()->le()->bonded());
}

TEST_F(PeerCacheTestBondingTest, BondsUpdatedWhenNewServicesAdded) {
  ASSERT_TRUE(NewPeer(kAddrBrEdr, true));
  ASSERT_EQ(peer(), cache()->FindByAddress(kAddrBrEdr));
  ASSERT_FALSE(peer()->bonded());

  ASSERT_FALSE(kBrEdrKey.security().secure_connections());
  EXPECT_TRUE(cache()->StoreBrEdrBond(kAddrBrEdr, kBrEdrKey));
  EXPECT_TRUE(peer()->bredr()->bonded());
  EXPECT_EQ(1, bonded_callback_count());

  peer()->MutBrEdr().AddService(UUID());
  EXPECT_EQ(2, bonded_callback_count());
}

TEST_F(PeerCacheTestBondingTest, RemoveDisconnectedPeerOnUnknownPeer) {
  const PeerId id(0x9999);
  ASSERT_FALSE(cache()->FindById(id));
  EXPECT_TRUE(cache()->RemoveDisconnectedPeer(id));
  EXPECT_EQ(0, updated_callback_count());
}

TEST_F(PeerCacheTestBondingTest, RemoveDisconnectedPeerOnUnconnectedPeer) {
  ASSERT_FALSE(peer()->connected());
  const PeerId id = peer()->identifier();
  EXPECT_TRUE(cache()->RemoveDisconnectedPeer(id));
  EXPECT_EQ(1, removed_callback_count());
  EXPECT_FALSE(cache()->FindById(id));
}

TEST_F(PeerCacheTestBondingTest, RemoveDisconnectedPeerOnConnectedPeer) {
  Peer::ConnectionToken conn_token = peer()->MutLe().RegisterConnection();
  ASSERT_TRUE(peer()->connected());
  const PeerId id = peer()->identifier();
  EXPECT_FALSE(cache()->RemoveDisconnectedPeer(id));
  EXPECT_EQ(0, removed_callback_count());
  EXPECT_TRUE(cache()->FindById(id));
}

// Fixture parameterized by peer address
class DualModeBondingTest
    : public PeerCacheTestBondingTest,
      public ::testing::WithParamInterface<DeviceAddress> {};

TEST_P(DualModeBondingTest, AddBondedPeerSuccess) {
  sm::PairingData data;
  data.peer_ltk = kLTK;
  data.local_ltk = kLTK;

  const DeviceAddress& address = GetParam();
  EXPECT_TRUE(
      cache()->AddBondedPeer(BondingData{.identifier = kId,
                                         .address = address,
                                         .name = kName,
                                         .device_class = kTestDeviceClass,
                                         .le_pairing_data = data,
                                         .bredr_link_key = kBrEdrKey,
                                         .bredr_services = kBrEdrServices}));
  auto* peer = cache()->FindById(kId);
  ASSERT_TRUE(peer);
  EXPECT_EQ(peer, cache()->FindByAddress(kAddrLeAlias));
  EXPECT_EQ(peer, cache()->FindByAddress(kAddrBrEdr));
  EXPECT_EQ(kId, peer->identifier());
  EXPECT_EQ(address, peer->address());
  EXPECT_EQ(kName, peer->name());
  EXPECT_EQ(Peer::NameSource::kUnknown, peer->name_source());
  EXPECT_TRUE(peer->identity_known());
  EXPECT_TRUE(peer->bonded());
  ASSERT_TRUE(peer->le());
  EXPECT_TRUE(peer->le()->bonded());
  ASSERT_TRUE(peer->le()->bond_data());
  EXPECT_EQ(data, *peer->le()->bond_data());
  ASSERT_TRUE(peer->bredr());
  EXPECT_TRUE(peer->bredr()->bonded());
  ASSERT_TRUE(peer->bredr()->link_key());
  EXPECT_EQ(kBrEdrKey, *peer->bredr()->link_key());
  ASSERT_TRUE(peer->bredr()->device_class());
  EXPECT_EQ(kTestDeviceClass, *peer->bredr()->device_class());
  EXPECT_THAT(peer->bredr()->services(),
              testing::UnorderedElementsAreArray(kBrEdrServices));
  EXPECT_EQ(TechnologyType::kDualMode, peer->technology());

  // The "new bond" callback should not be called when restoring a previously
  // bonded peer.
  EXPECT_FALSE(bonded_callback_called());
}

// Test dual-mode character of peer using the same address of both types.
INSTANTIATE_TEST_SUITE_P(PeerCacheTest,
                         DualModeBondingTest,
                         ::testing::Values(kAddrBrEdr, kAddrLeAlias));

template <const DeviceAddress* DevAddr>
class PeerCacheTest_UpdateCallbackTest : public PeerCacheTest {
 public:
  void SetUp() override {
    PeerCacheTest::SetUp();

    was_called_ = false;
    ASSERT_TRUE(NewPeer(*DevAddr, true));
    cache()->add_peer_updated_callback(
        [this](const auto&) { was_called_ = true; });
    ir_.view().bd_addr().CopyFrom(peer()->address().value().view());
    irr_.view().bd_addr().CopyFrom(peer()->address().value().view());
    extended_inquiry_result_event_.view().bd_addr().CopyFrom(
        peer()->address().value().view());
    eir_data().SetToZeros();
    EXPECT_FALSE(was_called_);
  }

  void TearDown() override { PeerCacheTest::TearDown(); }

 protected:
  pw::bluetooth::emboss::InquiryResultWriter ir() { return ir_.view(); }
  pw::bluetooth::emboss::InquiryResultWithRssiWriter irr() {
    return irr_.view();
  }
  pw::bluetooth::emboss::ExtendedInquiryResultEventWriter
  extended_inquiry_result_event() {
    return extended_inquiry_result_event_.view();
  }

  MutableBufferView eir_data() {
    return MutableBufferView(extended_inquiry_result_event_.view()
                                 .extended_inquiry_response()
                                 .BackingStorage()
                                 .data(),
                             extended_inquiry_result_event_.view()
                                 .extended_inquiry_response()
                                 .SizeInBytes());
  }
  bool was_called() const { return was_called_; }
  void ClearWasCalled() { was_called_ = false; }

 private:
  bool was_called_;
  StaticPacket<pw::bluetooth::emboss::InquiryResultWriter> ir_;
  StaticPacket<pw::bluetooth::emboss::InquiryResultWithRssiWriter> irr_;
  StaticPacket<pw::bluetooth::emboss::ExtendedInquiryResultEventWriter>
      extended_inquiry_result_event_;
};

using PeerCacheBrEdrUpdateCallbackTest =
    PeerCacheTest_UpdateCallbackTest<&kAddrBrEdr>;
using PeerCacheLowEnergyUpdateCallbackTest =
    PeerCacheTest_UpdateCallbackTest<&kAddrLeAlias>;

TEST_F(PeerCacheLowEnergyUpdateCallbackTest,
       ChangingLEConnectionStateTriggersUpdateCallback) {
  Peer::ConnectionToken conn_token = peer()->MutLe().RegisterConnection();
  EXPECT_TRUE(was_called());
}

TEST_F(PeerCacheLowEnergyUpdateCallbackTest,
       SetAdvertisingDataTriggersUpdateCallbackOnNameSet) {
  peer()->MutLe().SetAdvertisingData(
      kTestRSSI, kAdvData, pw::chrono::SystemClock::time_point());
  EXPECT_TRUE(was_called());
  ASSERT_TRUE(peer()->name());
  EXPECT_EQ("Test", *peer()->name());
  EXPECT_EQ(Peer::NameSource::kAdvertisingDataComplete, *peer()->name_source());
}

TEST_F(PeerCacheLowEnergyUpdateCallbackTest,
       SetLowEnergyAdvertisingDataUpdateCallbackProvidesUpdatedPeer) {
  ASSERT_NE(peer()->rssi(), kTestRSSI);
  cache()->add_peer_updated_callback([&](const auto& updated_peer) {
    ASSERT_TRUE(updated_peer.le());
    ASSERT_TRUE(updated_peer.name().has_value());
    EXPECT_EQ(updated_peer.name().value(), "Test");
    EXPECT_EQ(updated_peer.rssi(), kTestRSSI);
  });
  peer()->MutLe().SetAdvertisingData(
      kTestRSSI, kAdvData, pw::chrono::SystemClock::time_point());
}

TEST_F(PeerCacheLowEnergyUpdateCallbackTest,
       SetAdvertisingDataTriggersUpdateCallbackOnSameNameAndRssi) {
  peer()->MutLe().SetAdvertisingData(
      kTestRSSI, kAdvData, pw::chrono::SystemClock::time_point());
  ASSERT_TRUE(was_called());

  ClearWasCalled();
  peer()->MutLe().SetAdvertisingData(
      kTestRSSI, kAdvData, pw::chrono::SystemClock::time_point());
  EXPECT_TRUE(was_called());
}

TEST_F(PeerCacheLowEnergyUpdateCallbackTest,
       SetLowEnergyConnectionParamsDoesNotTriggerUpdateCallback) {
  peer()->MutLe().SetConnectionParameters({});
  EXPECT_FALSE(was_called());
}

TEST_F(PeerCacheLowEnergyUpdateCallbackTest,
       SetLowEnergyPreferredConnectionParamsDoesNotTriggerUpdateCallback) {
  peer()->MutLe().SetPreferredConnectionParameters({});
  EXPECT_FALSE(was_called());
}

TEST_F(PeerCacheLowEnergyUpdateCallbackTest,
       BecomingDualModeTriggersUpdateCallBack) {
  EXPECT_EQ(TechnologyType::kLowEnergy, peer()->technology());

  size_t call_count = 0;
  cache()->add_peer_updated_callback([&](const auto&) { ++call_count; });
  peer()->MutBrEdr();
  EXPECT_EQ(TechnologyType::kDualMode, peer()->technology());
  EXPECT_EQ(call_count, 1U);

  // Calling MutBrEdr again on doesn't trigger additional callbacks.
  peer()->MutBrEdr();
  EXPECT_EQ(call_count, 1U);
  peer()->MutBrEdr().SetInquiryData(extended_inquiry_result_event());
  EXPECT_EQ(call_count, 2U);
}

TEST_F(PeerCacheBrEdrUpdateCallbackTest,
       ChangingBrEdrConnectionStateTriggersUpdateCallback) {
  Peer::ConnectionToken token = peer()->MutBrEdr().RegisterConnection();
  EXPECT_TRUE(was_called());
}

TEST_F(
    PeerCacheBrEdrUpdateCallbackTest,
    SetBrEdrInquiryDataFromInquiryResultTriggersUpdateCallbackOnPeerClassSet) {
  ir().class_of_device().BackingStorage().WriteUInt(kTestDeviceClass.to_int());
  peer()->MutBrEdr().SetInquiryData(ir());
  EXPECT_TRUE(was_called());
}

TEST_F(PeerCacheBrEdrUpdateCallbackTest,
       SetBrEdrInquiryDataFromInquiryResultUpdateCallbackProvidesUpdatedPeer) {
  ir().class_of_device().BackingStorage().WriteUInt(kTestDeviceClass.to_int());
  cache()->add_peer_updated_callback([](const auto& updated_peer) {
    ASSERT_TRUE(updated_peer.bredr());
    ASSERT_TRUE(updated_peer.bredr()->device_class());
    EXPECT_EQ(DeviceClass::MajorClass(0x02),
              updated_peer.bredr()->device_class()->major_class());
  });
  peer()->MutBrEdr().SetInquiryData(ir());
}

TEST_F(
    PeerCacheBrEdrUpdateCallbackTest,
    SetBrEdrInquiryDataFromInquiryResultDoesNotTriggerUpdateCallbackOnSameDeviceClass) {
  ir().class_of_device().BackingStorage().WriteUInt(kTestDeviceClass.to_int());
  peer()->MutBrEdr().SetInquiryData(ir());
  ASSERT_TRUE(was_called());

  ClearWasCalled();
  peer()->MutBrEdr().SetInquiryData(ir());
  EXPECT_FALSE(was_called());
}

TEST_F(
    PeerCacheBrEdrUpdateCallbackTest,
    SetBrEdrInquiryDataFromInquiryResultRSSITriggersUpdateCallbackOnDeviceClassSet) {
  irr().class_of_device().major_device_class().Write(
      pw::bluetooth::emboss::MajorDeviceClass::PHONE);
  peer()->MutBrEdr().SetInquiryData(irr());
  EXPECT_TRUE(was_called());
}

TEST_F(
    PeerCacheBrEdrUpdateCallbackTest,
    SetBrEdrInquiryDataFromInquiryResultRSSIUpdateCallbackProvidesUpdatedPeer) {
  irr().class_of_device().major_device_class().Write(
      pw::bluetooth::emboss::MajorDeviceClass::PHONE);
  cache()->add_peer_updated_callback([](const auto& updated_peer) {
    ASSERT_TRUE(updated_peer.bredr()->device_class());
    EXPECT_EQ(DeviceClass::MajorClass::kPhone,
              updated_peer.bredr()->device_class()->major_class());
  });
  peer()->MutBrEdr().SetInquiryData(irr());
}

TEST_F(
    PeerCacheBrEdrUpdateCallbackTest,
    SetBrEdrInquiryDataFromInquiryResultRSSIDoesNotTriggerUpdateCallbackOnSameDeviceClass) {
  irr().class_of_device().major_device_class().Write(
      pw::bluetooth::emboss::MajorDeviceClass::PHONE);
  peer()->MutBrEdr().SetInquiryData(irr());
  ASSERT_TRUE(was_called());

  ClearWasCalled();
  peer()->MutBrEdr().SetInquiryData(irr());
  EXPECT_FALSE(was_called());
}

TEST_F(
    PeerCacheBrEdrUpdateCallbackTest,
    SetBrEdrInquiryDataFromInquiryResultRSSIDoesNotTriggerUpdateCallbackOnRSSI) {
  irr().rssi().Write(1);
  peer()->MutBrEdr().SetInquiryData(irr());
  ASSERT_TRUE(was_called());  // Callback due to |class_of_device|.

  ClearWasCalled();
  irr().rssi().Write(20);
  peer()->MutBrEdr().SetInquiryData(irr());
  EXPECT_FALSE(was_called());
}

TEST_F(
    PeerCacheBrEdrUpdateCallbackTest,
    SetBrEdrInquiryDataFromExtendedInquiryResultEventParamsTriggersUpdateCallbackOnDeviceClassSet) {
  extended_inquiry_result_event().class_of_device().major_device_class().Write(
      pw::bluetooth::emboss::MajorDeviceClass::PHONE);
  peer()->MutBrEdr().SetInquiryData(extended_inquiry_result_event());
  EXPECT_TRUE(was_called());
}

TEST_F(
    PeerCacheBrEdrUpdateCallbackTest,
    SetBrEdrInquiryDataFromExtendedInquiryResultEventParamsTriggersUpdateCallbackOnNameSet) {
  peer()->MutBrEdr().SetInquiryData(extended_inquiry_result_event());
  ASSERT_TRUE(was_called());  // Callback due to |class_of_device|.

  ClearWasCalled();
  eir_data().Write(kEirData);
  peer()->MutBrEdr().SetInquiryData(extended_inquiry_result_event());
  EXPECT_TRUE(was_called());
}

TEST_F(
    PeerCacheBrEdrUpdateCallbackTest,
    SetBrEdrInquiryDataFromExtendedInquiryResultEventParamsUpdateCallbackProvidesUpdatedPeer) {
  extended_inquiry_result_event().clock_offset().valid().Write(true);
  extended_inquiry_result_event().clock_offset().clock_offset().Write(1);
  extended_inquiry_result_event().page_scan_repetition_mode().Write(
      pw::bluetooth::emboss::PageScanRepetitionMode::R1_);
  extended_inquiry_result_event().rssi().Write(kTestRSSI);
  extended_inquiry_result_event().class_of_device().major_device_class().Write(
      pw::bluetooth::emboss::MajorDeviceClass::PHONE);
  eir_data().Write(kEirData);
  ASSERT_FALSE(peer()->name().has_value());
  ASSERT_EQ(peer()->rssi(), hci_spec::kRSSIInvalid);
  cache()->add_peer_updated_callback([](const auto& updated_peer) {
    const auto& data = updated_peer.bredr();
    ASSERT_TRUE(data);
    ASSERT_TRUE(data->clock_offset().has_value());
    ASSERT_TRUE(data->page_scan_repetition_mode().has_value());
    ASSERT_TRUE(data->device_class().has_value());
    ASSERT_TRUE(updated_peer.name().has_value());

    EXPECT_EQ(*data->clock_offset(), 0x01);
    EXPECT_EQ(*data->page_scan_repetition_mode(),
              pw::bluetooth::emboss::PageScanRepetitionMode::R1_);
    EXPECT_EQ(DeviceClass::MajorClass(0x02),
              updated_peer.bredr()->device_class()->major_class());
    EXPECT_EQ(updated_peer.rssi(), kTestRSSI);
    EXPECT_EQ(*updated_peer.name(), "Test");
    EXPECT_EQ(*updated_peer.name_source(),
              Peer::NameSource::kInquiryResultComplete);
  });
  peer()->MutBrEdr().SetInquiryData(extended_inquiry_result_event());
}

TEST_F(
    PeerCacheBrEdrUpdateCallbackTest,
    SetBrEdrInquiryDataFromExtendedInquiryResultEventParamsGeneratesExactlyOneUpdateCallbackRegardlessOfNumberOfFieldsChanged) {
  extended_inquiry_result_event().clock_offset().valid().Write(true);
  extended_inquiry_result_event().clock_offset().clock_offset().Write(1);
  extended_inquiry_result_event().page_scan_repetition_mode().Write(
      pw::bluetooth::emboss::PageScanRepetitionMode::R1_);
  extended_inquiry_result_event().rssi().Write(kTestRSSI);
  extended_inquiry_result_event().class_of_device().major_device_class().Write(
      pw::bluetooth::emboss::MajorDeviceClass::PHONE);

  size_t call_count = 0;
  cache()->add_peer_updated_callback([&](const auto&) { ++call_count; });
  peer()->MutBrEdr().SetInquiryData(extended_inquiry_result_event());
  EXPECT_EQ(call_count, 1U);
}

TEST_F(
    PeerCacheBrEdrUpdateCallbackTest,
    SetBrEdrInquiryDataFromExtendedInquiryResultEventParamsDoesNotTriggerUpdateCallbackOnSamePeerClass) {
  extended_inquiry_result_event().class_of_device().major_device_class().Write(
      pw::bluetooth::emboss::MajorDeviceClass::PHONE);
  peer()->MutBrEdr().SetInquiryData(extended_inquiry_result_event());
  ASSERT_TRUE(was_called());

  ClearWasCalled();
  peer()->MutBrEdr().SetInquiryData(extended_inquiry_result_event());
  EXPECT_FALSE(was_called());
}

TEST_F(
    PeerCacheBrEdrUpdateCallbackTest,
    SetBrEdrInquiryDataFromExtendedInquiryResultEventParamsDoesNotTriggerUpdateCallbackOnSameName) {
  eir_data().Write(kEirData);
  peer()->MutBrEdr().SetInquiryData(extended_inquiry_result_event());
  ASSERT_TRUE(was_called());

  ClearWasCalled();
  peer()->MutBrEdr().SetInquiryData(extended_inquiry_result_event());
  EXPECT_FALSE(was_called());
}

TEST_F(
    PeerCacheBrEdrUpdateCallbackTest,
    SetBrEdrInquiryDataFromExtendedInquiryResultEventParamsDoesNotTriggerUpdateCallbackOnRSSI) {
  extended_inquiry_result_event().rssi().Write(1);
  peer()->MutBrEdr().SetInquiryData(extended_inquiry_result_event());
  ASSERT_TRUE(was_called());  // Callback due to |class_of_device|.

  ClearWasCalled();
  extended_inquiry_result_event().rssi().Write(20);
  peer()->MutBrEdr().SetInquiryData(extended_inquiry_result_event());
  EXPECT_FALSE(was_called());
}

TEST_F(PeerCacheBrEdrUpdateCallbackTest, RegisterNameTriggersUpdateCallback) {
  peer()->RegisterName("nombre");
  EXPECT_TRUE(was_called());
}

TEST_F(PeerCacheBrEdrUpdateCallbackTest,
       RegisterNameDoesNotTriggerUpdateCallbackOnSameName) {
  peer()->RegisterName("nombre");
  ASSERT_TRUE(was_called());

  bool was_called_again = false;
  cache()->add_peer_updated_callback(
      [&](const auto&) { was_called_again = true; });
  peer()->RegisterName("nombre");
  EXPECT_FALSE(was_called_again);
}

TEST_F(PeerCacheBrEdrUpdateCallbackTest,
       BecomingDualModeTriggersUpdateCallBack) {
  EXPECT_EQ(TechnologyType::kClassic, peer()->technology());

  size_t call_count = 0;
  cache()->add_peer_updated_callback([&](const auto&) { ++call_count; });
  peer()->MutLe();
  EXPECT_EQ(TechnologyType::kDualMode, peer()->technology());
  EXPECT_EQ(call_count, 1U);

  // Calling MutLe again doesn't trigger additional callbacks.
  peer()->MutLe();
  EXPECT_EQ(call_count, 1U);
  peer()->MutLe().SetAdvertisingData(
      kTestRSSI, kAdvData, pw::chrono::SystemClock::time_point());
  EXPECT_EQ(call_count, 2U);
}

class PeerCacheExpirationTest : public pw::async::test::FakeDispatcherFixture {
 public:
  PeerCacheExpirationTest() = default;
  void SetUp() override {
    cache_.set_peer_removed_callback([this](PeerId) { peers_removed_++; });
    auto* peer = cache_.NewPeer(kAddrLeAlias, /*connectable=*/true);
    ASSERT_TRUE(peer);
    ASSERT_TRUE(peer->temporary());
    peer_addr_ = peer->address();
    peer_addr_alias_ = kAddrBrEdr;
    peer_id_ = peer->identifier();
    peers_removed_ = 0;
  }

  void TearDown() override {
    cache_.set_peer_removed_callback(nullptr);
    RunUntilIdle();
  }

  Peer* GetDefaultPeer() { return cache_.FindById(peer_id_); }
  Peer* GetPeerById(PeerId id) { return cache_.FindById(id); }
  bool IsDefaultPeerAddressInCache() const {
    return cache_.FindByAddress(peer_addr_);
  }
  bool IsOtherTransportAddressInCache() const {
    return cache_.FindByAddress(peer_addr_alias_);
  }
  bool IsDefaultPeerPresent() { return GetDefaultPeer(); }
  Peer* NewPeer(const DeviceAddress& address, bool connectable) {
    return cache_.NewPeer(address, connectable);
  }
  int peers_removed() const { return peers_removed_; }

 private:
  PeerCache cache_{dispatcher()};
  DeviceAddress peer_addr_;
  DeviceAddress peer_addr_alias_;
  PeerId peer_id_;
  int peers_removed_;
};

TEST_F(PeerCacheExpirationTest, TemporaryDiesSixtySecondsAfterBirth) {
  RunFor(kCacheTimeout);
  EXPECT_FALSE(IsDefaultPeerPresent());
  EXPECT_EQ(1, peers_removed());
}

TEST_F(PeerCacheExpirationTest, TemporaryLivesForSixtySecondsAfterBirth) {
  RunFor(kCacheTimeout - std::chrono::milliseconds(1));
  EXPECT_TRUE(IsDefaultPeerPresent());
  EXPECT_EQ(0, peers_removed());
}

TEST_F(PeerCacheExpirationTest, TemporaryLivesForSixtySecondsSinceLastSeen) {
  RunFor(kCacheTimeout - std::chrono::milliseconds(1));
  ASSERT_TRUE(IsDefaultPeerPresent());

  // Tickle peer, and verify it sticks around for another cache timeout.
  GetDefaultPeer()->RegisterName("nombre");
  RunFor(kCacheTimeout - std::chrono::milliseconds(1));
  EXPECT_TRUE(IsDefaultPeerPresent());
}

TEST_F(PeerCacheExpirationTest, TemporaryDiesSixtySecondsAfterLastSeen) {
  RunFor(kCacheTimeout - std::chrono::milliseconds(1));
  ASSERT_TRUE(IsDefaultPeerPresent());

  // Tickle peer, and verify it expires after cache timeout.
  GetDefaultPeer()->RegisterName("nombre");
  RunFor(kCacheTimeout);
  EXPECT_FALSE(IsDefaultPeerPresent());
}

TEST_F(PeerCacheExpirationTest, CanMakeNonTemporaryJustBeforeSixtySeconds) {
  // At last possible moment, make peer non-temporary,
  RunFor(kCacheTimeout - std::chrono::milliseconds(1));
  ASSERT_TRUE(IsDefaultPeerPresent());
  Peer::ConnectionToken conn_token =
      GetDefaultPeer()->MutLe().RegisterConnection();
  ASSERT_FALSE(GetDefaultPeer()->temporary());

  // Verify that the peer survives.
  RunFor(kCacheTimeout * 10);
  EXPECT_TRUE(IsDefaultPeerPresent());
}

TEST_F(PeerCacheExpirationTest, LEConnectedPeerLivesMuchMoreThanSixtySeconds) {
  ASSERT_TRUE(IsDefaultPeerPresent());
  Peer::ConnectionToken conn_token =
      GetDefaultPeer()->MutLe().RegisterConnection();
  RunFor(kCacheTimeout * 10);
  ASSERT_TRUE(IsDefaultPeerPresent());
  EXPECT_FALSE(GetDefaultPeer()->temporary());
}

TEST_F(PeerCacheExpirationTest,
       BREDRConnectedPeerLivesMuchMoreThanSixtySeconds) {
  ASSERT_TRUE(IsDefaultPeerPresent());
  Peer::ConnectionToken token =
      GetDefaultPeer()->MutBrEdr().RegisterConnection();
  RunFor(kCacheTimeout * 10);
  ASSERT_TRUE(IsDefaultPeerPresent());
  EXPECT_FALSE(GetDefaultPeer()->temporary());
}

TEST_F(PeerCacheExpirationTest, LePeerBecomesNonTemporaryWhenConnecting) {
  ASSERT_TRUE(IsDefaultPeerPresent());
  ASSERT_EQ(kAddrLeAlias, GetDefaultPeer()->address());
  ASSERT_TRUE(GetDefaultPeer()->temporary());

  Peer::InitializingConnectionToken init_token =
      GetDefaultPeer()->MutLe().RegisterInitializingConnection();
  EXPECT_FALSE(GetDefaultPeer()->temporary());

  RunFor(kCacheTimeout);
  ASSERT_TRUE(IsDefaultPeerPresent());
}

TEST_F(PeerCacheExpirationTest, LEPublicPeerRemainsNonTemporaryOnDisconnect) {
  ASSERT_TRUE(IsDefaultPeerPresent());
  ASSERT_EQ(kAddrLeAlias, GetDefaultPeer()->address());
  {
    Peer::ConnectionToken conn_token =
        GetDefaultPeer()->MutLe().RegisterConnection();
    ASSERT_FALSE(GetDefaultPeer()->temporary());

    RunFor(kCacheTimeout + std::chrono::seconds(1));
    ASSERT_TRUE(IsDefaultPeerPresent());
    ASSERT_TRUE(GetDefaultPeer()->identity_known());
    // Destroy conn_token at end of scope
  }
  EXPECT_FALSE(GetDefaultPeer()->temporary());

  RunFor(kCacheTimeout);
  EXPECT_TRUE(IsDefaultPeerPresent());
}

TEST_F(PeerCacheExpirationTest, LERandomPeerBecomesTemporaryOnDisconnect) {
  // Create our Peer, and get it into the kConnected state.
  PeerId custom_peer_id;
  std::optional<Peer::ConnectionToken> conn_token;
  {
    auto* custom_peer = NewPeer(kAddrLeRandom, /*connectable=*/true);
    ASSERT_TRUE(custom_peer);
    ASSERT_TRUE(custom_peer->temporary());
    ASSERT_FALSE(custom_peer->identity_known());
    custom_peer_id = custom_peer->identifier();

    conn_token = custom_peer->MutLe().RegisterConnection();
    ASSERT_FALSE(custom_peer->temporary());
    ASSERT_FALSE(custom_peer->identity_known());
  }

  // Verify that the connected peer does not expire out of the cache.
  // Then disconnect the peer, in preparation for the next stage of our test.
  {
    EXPECT_EQ(0, peers_removed());
    RunFor(std::chrono::seconds(61));
    EXPECT_EQ(1, peers_removed());  // Default peer timed out.
    auto* custom_peer = GetPeerById(custom_peer_id);
    ASSERT_TRUE(custom_peer);
    ASSERT_FALSE(custom_peer->identity_known());

    conn_token.reset();
    EXPECT_TRUE(custom_peer->temporary());
    EXPECT_FALSE(custom_peer->identity_known());
  }

  // Verify that the disconnected peer expires out of the cache.
  RunFor(std::chrono::seconds(61));
  EXPECT_FALSE(GetPeerById(custom_peer_id));
  EXPECT_EQ(2, peers_removed());
}

TEST_F(PeerCacheExpirationTest, BrEdrPeerRemainsNonTemporaryOnDisconnect) {
  // Create our Peer, and get it into the kConnected state.
  PeerId custom_peer_id;
  std::optional<Peer::ConnectionToken> conn_token;
  {
    auto* custom_peer = NewPeer(kAddrLePublic, /*connectable=*/true);
    ASSERT_TRUE(custom_peer);
    conn_token = custom_peer->MutLe().RegisterConnection();
    custom_peer_id = custom_peer->identifier();
  }

  // Verify that the connected peer does not expire out of the cache.
  // Then disconnect the peer, in preparation for the next stage of our test.
  {
    EXPECT_EQ(0, peers_removed());
    RunFor(kCacheTimeout * 10);
    EXPECT_EQ(1, peers_removed());  // Default peer timed out.
    auto* custom_peer = GetPeerById(custom_peer_id);
    ASSERT_TRUE(custom_peer);
    ASSERT_TRUE(custom_peer->identity_known());
    EXPECT_FALSE(custom_peer->temporary());

    conn_token.reset();
    ASSERT_TRUE(GetPeerById(custom_peer_id));
    EXPECT_FALSE(custom_peer->temporary());
  }

  // Verify that the disconnected peer does _not_ expire out of the cache.
  // We expect the peer to remain, because BrEdr peers are non-temporary
  // even when disconnected.
  RunFor(kCacheTimeout);
  EXPECT_TRUE(GetPeerById(custom_peer_id));
  EXPECT_EQ(1, peers_removed());
}

TEST_F(PeerCacheExpirationTest, ExpirationUpdatesAddressMap) {
  ASSERT_TRUE(IsDefaultPeerAddressInCache());
  ASSERT_TRUE(IsOtherTransportAddressInCache());
  RunFor(kCacheTimeout);
  EXPECT_FALSE(IsDefaultPeerAddressInCache());
  EXPECT_FALSE(IsOtherTransportAddressInCache());
}

TEST_F(PeerCacheExpirationTest, SetAdvertisingDataUpdatesExpiration) {
  RunFor(kCacheTimeout - std::chrono::milliseconds(1));
  ASSERT_TRUE(IsDefaultPeerPresent());
  GetDefaultPeer()->MutLe().SetAdvertisingData(
      kTestRSSI, StaticByteBuffer<1>{}, pw::chrono::SystemClock::time_point());
  RunFor(kCacheTimeout - std::chrono::milliseconds(1));
  EXPECT_TRUE(IsDefaultPeerPresent());
  // Setting advertising data with the same rssi & name should also update the
  // expiry.
  GetDefaultPeer()->MutLe().SetAdvertisingData(
      kTestRSSI, StaticByteBuffer<1>{}, pw::chrono::SystemClock::time_point());
  RunFor(std::chrono::milliseconds(1));
  EXPECT_TRUE(IsDefaultPeerPresent());
}

TEST_F(PeerCacheExpirationTest,
       SetBrEdrInquiryDataFromInquiryResultUpdatesExpiration) {
  StaticPacket<pw::bluetooth::emboss::InquiryResultWriter> ir;
  ASSERT_TRUE(IsDefaultPeerPresent());
  ir.view().bd_addr().CopyFrom(GetDefaultPeer()->address().value().view());

  RunFor(kCacheTimeout - std::chrono::milliseconds(1));
  ASSERT_TRUE(IsDefaultPeerPresent());
  GetDefaultPeer()->MutBrEdr().SetInquiryData(ir.view());

  RunFor(std::chrono::milliseconds(1));
  EXPECT_TRUE(IsDefaultPeerPresent());
}

TEST_F(PeerCacheExpirationTest,
       SetBrEdrInquiryDataFromInquiryResultRSSIUpdatesExpiration) {
  StaticPacket<pw::bluetooth::emboss::InquiryResultWithRssiWriter> irr;
  ASSERT_TRUE(IsDefaultPeerPresent());
  irr.view().bd_addr().CopyFrom(GetDefaultPeer()->address().value().view());

  RunFor(kCacheTimeout - std::chrono::milliseconds(1));
  ASSERT_TRUE(IsDefaultPeerPresent());
  GetDefaultPeer()->MutBrEdr().SetInquiryData(irr.view());

  RunFor(std::chrono::milliseconds(1));
  EXPECT_TRUE(IsDefaultPeerPresent());
}

TEST_F(
    PeerCacheExpirationTest,
    SetBrEdrInquiryDataFromExtendedInquiryResultEventParamsUpdatesExpiration) {
  StaticPacket<pw::bluetooth::emboss::ExtendedInquiryResultEventWriter> event;
  ASSERT_TRUE(IsDefaultPeerPresent());
  event.view().bd_addr().CopyFrom(GetDefaultPeer()->address().value().view());

  RunFor(kCacheTimeout - std::chrono::milliseconds(1));
  ASSERT_TRUE(IsDefaultPeerPresent());
  GetDefaultPeer()->MutBrEdr().SetInquiryData(event.view());

  RunFor(std::chrono::milliseconds(1));
  EXPECT_TRUE(IsDefaultPeerPresent());
}

TEST_F(PeerCacheExpirationTest, RegisterNameUpdatesExpiration) {
  RunFor(kCacheTimeout - std::chrono::milliseconds(1));
  ASSERT_TRUE(IsDefaultPeerPresent());
  GetDefaultPeer()->RegisterName({});
  RunFor(std::chrono::milliseconds(1));
  EXPECT_TRUE(IsDefaultPeerPresent());
}

}  // namespace
}  // namespace bt::gap
