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

#include "pw_bluetooth_sapphire/internal/host/gatt/gatt.h"

#include <pw_async/fake_dispatcher_fixture.h>

#include "pw_bluetooth_sapphire/internal/host/att/att.h"
#include "pw_bluetooth_sapphire/internal/host/att/error.h"
#include "pw_bluetooth_sapphire/internal/host/common/host_error.h"
#include "pw_bluetooth_sapphire/internal/host/common/identifier.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/fake_client.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/gatt_defs.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/local_service_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/mock_server.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/fake_channel.h"
#include "pw_bluetooth_sapphire/internal/host/testing/inspect_util.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"

namespace bt::gatt::internal {
namespace {

#ifndef NINSPECT
using namespace inspect::testing;
using bt::testing::GetInspectValue;
using bt::testing::ReadInspect;
#endif

constexpr PeerId kPeerId0(0);
constexpr PeerId kPeerId1(1);
constexpr PeerId kPeerId(10);
constexpr UUID kTestServiceUuid0(uint16_t{0xbeef});
constexpr IdType kChrcId{13};
constexpr bt::UUID kChrcUuid(uint16_t{113u});

// Factory function for tests of client-facing behavior that don't care about
// the server
std::unique_ptr<Server> CreateMockServer(
    PeerId peer_id, LocalServiceManager::WeakPtr local_services) {
  return std::make_unique<testing::MockServer>(peer_id,
                                               std::move(local_services));
}

class GattTest : public pw::async::test::FakeDispatcherFixture {
 public:
  GattTest() = default;
  ~GattTest() override = default;

 protected:
  struct ServiceWatcherData {
    PeerId peer_id;
    std::vector<att::Handle> removed;
    ServiceList added;
    ServiceList modified;
  };

  void SetUp() override {
    auto client = std::make_unique<testing::FakeClient>(dispatcher());
    fake_client_weak_ = client->AsFakeWeakPtr();
    client_ = std::move(client);
    gatt_ = GATT::Create();
    gatt_->AttachInspect(gatt_inspector_.GetRoot(), "gatt");
  }

  void TearDown() override {
    // Clear any previous expectations that are based on the ATT Write Request,
    // so that write requests sent during RemoteService::ShutDown() are ignored.
    fake_client()->set_write_request_callback({});
    gatt_.reset();
  }

  // Register an arbitrary service with a single characteristic of id |kChrcId|,
  // e.g. for sending notifications. Returns the internal IdType of the
  // registered service.
  IdType RegisterArbitraryService() {
    auto svc = std::make_unique<Service>(/*primary=*/true, kTestServiceUuid0);
    const att::AccessRequirements kReadPerm,
        kWritePerm;  // Default is not allowed
    // Allow "update" (i.e. indications / notifications) with no security
    const att::AccessRequirements kUpdatePerm(/*encryption=*/false,
                                              /*authentication=*/false,
                                              /*authorization=*/false);
    auto chrc = std::make_unique<Characteristic>(kChrcId,
                                                 kChrcUuid,
                                                 Property::kIndicate,
                                                 /*extended_properties=*/0,
                                                 kReadPerm,
                                                 kWritePerm,
                                                 kUpdatePerm);
    svc->AddCharacteristic(std::move(chrc));

    std::optional<IdType> svc_id = std::nullopt;
    auto id_cb = [&svc_id](IdType received_id) {
      EXPECT_NE(kInvalidId, received_id);
      svc_id = received_id;
    };
    gatt()->RegisterService(std::move(svc),
                            std::move(id_cb),
                            NopReadHandler,
                            NopWriteHandler,
                            NopCCCallback);
    RunUntilIdle();
    EXPECT_TRUE(svc_id.has_value());
    return *svc_id;
  }

  GATT* gatt() const { return gatt_.get(); }

  testing::FakeClient::WeakPtr fake_client() const { return fake_client_weak_; }
  std::unique_ptr<Client> take_client() { return std::move(client_); }

#ifndef NINSPECT
  inspect::Hierarchy ReadGattInspect() { return ReadInspect(gatt_inspector_); }

  uint64_t InspectProfileDiscoveryFailureCount() {
    std::optional<uint64_t> val = GetInspectValue<inspect::UintPropertyValue>(
        gatt_inspector_, {"gatt", "profile_discovery_failure_count"});
    PW_CHECK(val);
    return *val;
  }
#endif

 private:
  std::unique_ptr<GATT> gatt_;
  std::unique_ptr<Client> client_;
  testing::FakeClient::WeakPtr fake_client_weak_;
  inspect::Inspector gatt_inspector_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(GattTest);
};

TEST_F(GattTest, RemoteServiceWatcherNotifiesAddedModifiedAndRemovedService) {
  const att::Handle kGattSvcStartHandle(1);
  const att::Handle kSvcChangedChrcHandle(2);
  const att::Handle kSvcChangedChrcValueHandle(3);
  const att::Handle kCCCDescriptorHandle(4);
  const att::Handle kGattSvcEndHandle(kCCCDescriptorHandle);

  ServiceData gatt_svc(ServiceKind::PRIMARY,
                       kGattSvcStartHandle,
                       kGattSvcEndHandle,
                       types::kGenericAttributeService);
  CharacteristicData service_changed_chrc(Property::kIndicate,
                                          std::nullopt,
                                          kSvcChangedChrcHandle,
                                          kSvcChangedChrcValueHandle,
                                          types::kServiceChangedCharacteristic);
  DescriptorData ccc_descriptor(kCCCDescriptorHandle,
                                types::kClientCharacteristicConfig);
  fake_client()->set_services({gatt_svc});
  fake_client()->set_characteristics({service_changed_chrc});
  fake_client()->set_descriptors({ccc_descriptor});

  // Return success when a Service Changed Client Characteristic Config
  // descriptor write is performed.
  int write_request_count = 0;
  fake_client()->set_write_request_callback(
      [&](att::Handle handle, const auto&, auto status_callback) {
        write_request_count++;
        EXPECT_EQ(kCCCDescriptorHandle, handle);
        status_callback(fit::ok());
      });

  std::vector<ServiceWatcherData> svc_watcher_data;
  gatt()->RegisterRemoteServiceWatcherForPeer(
      kPeerId,
      [&](std::vector<att::Handle> removed,
          ServiceList added,
          ServiceList modified) {
        svc_watcher_data.push_back(
            ServiceWatcherData{.peer_id = kPeerId,
                               .removed = std::move(removed),
                               .added = std::move(added),
                               .modified = std::move(modified)});
      });

  gatt()->AddConnection(kPeerId, take_client(), CreateMockServer);
  RunUntilIdle();
  EXPECT_EQ(write_request_count, 0);

  gatt()->InitializeClient(kPeerId, /*service_uuids=*/{});
  RunUntilIdle();
  EXPECT_EQ(write_request_count, 1);
  ASSERT_EQ(1u, svc_watcher_data.size());
  ASSERT_EQ(1u, svc_watcher_data[0].added.size());
  EXPECT_EQ(kPeerId, svc_watcher_data[0].peer_id);
  EXPECT_EQ(kGattSvcStartHandle, svc_watcher_data[0].added[0]->handle());
#ifndef NINSPECT
  EXPECT_EQ(InspectProfileDiscoveryFailureCount(), 0u);
#endif

  // Add, modify, and remove a service.
  const att::Handle kSvc1StartHandle(5);
  const att::Handle kSvc1EndHandle(kSvc1StartHandle);

  // Add a test service to ensure that service discovery occurs after the
  // Service Changed characteristic is configured.
  ServiceData svc1(ServiceKind::PRIMARY,
                   kSvc1StartHandle,
                   kSvc1EndHandle,
                   kTestServiceUuid0);
  fake_client()->set_services({gatt_svc, svc1});

  // Send a notification that svc1 has been added.
  StaticByteBuffer svc_changed_range_buffer(
      LowerBits(kSvc1StartHandle),
      UpperBits(kSvc1StartHandle),  // start handle of affected range
      LowerBits(kSvc1EndHandle),
      UpperBits(kSvc1EndHandle)  // end handle of affected range
  );
  fake_client()->SendNotification(/*indicate=*/true,
                                  kSvcChangedChrcValueHandle,
                                  svc_changed_range_buffer,
                                  /*maybe_truncated=*/false);
  RunUntilIdle();
  ASSERT_EQ(2u, svc_watcher_data.size());
  ASSERT_EQ(1u, svc_watcher_data[1].added.size());
  EXPECT_EQ(0u, svc_watcher_data[1].removed.size());
  EXPECT_EQ(0u, svc_watcher_data[1].modified.size());
  EXPECT_EQ(kPeerId, svc_watcher_data[1].peer_id);
  EXPECT_EQ(kSvc1StartHandle, svc_watcher_data[1].added[0]->handle());
  bool original_service_removed = false;
  svc_watcher_data[1].added[0]->AddRemovedHandler(
      [&]() { original_service_removed = true; });

  // Send a notification that svc1 has been modified.
  fake_client()->SendNotification(/*indicate=*/true,
                                  kSvcChangedChrcValueHandle,
                                  svc_changed_range_buffer,
                                  /*maybe_truncated=*/false);
  RunUntilIdle();
  EXPECT_TRUE(original_service_removed);
  ASSERT_EQ(3u, svc_watcher_data.size());
  EXPECT_EQ(0u, svc_watcher_data[2].added.size());
  EXPECT_EQ(0u, svc_watcher_data[2].removed.size());
  ASSERT_EQ(1u, svc_watcher_data[2].modified.size());
  EXPECT_EQ(kPeerId, svc_watcher_data[2].peer_id);
  EXPECT_EQ(kSvc1StartHandle, svc_watcher_data[2].modified[0]->handle());
  bool modified_service_removed = false;
  svc_watcher_data[2].modified[0]->AddRemovedHandler(
      [&]() { modified_service_removed = true; });

  // Remove the service.
  fake_client()->set_services({gatt_svc});

  // Send a notification that svc1 has been removed.
  fake_client()->SendNotification(/*indicate=*/true,
                                  kSvcChangedChrcValueHandle,
                                  svc_changed_range_buffer,
                                  /*maybe_truncated=*/false);
  RunUntilIdle();
  EXPECT_TRUE(modified_service_removed);
  ASSERT_EQ(4u, svc_watcher_data.size());
  EXPECT_EQ(0u, svc_watcher_data[3].added.size());
  ASSERT_EQ(1u, svc_watcher_data[3].removed.size());
  EXPECT_EQ(0u, svc_watcher_data[3].modified.size());
  EXPECT_EQ(kPeerId, svc_watcher_data[3].peer_id);
  EXPECT_EQ(kSvc1StartHandle, svc_watcher_data[3].removed[0]);
}

// Register 3 service watchers, 2 of which are for the same peer.  Then
// unregister 2 service watchers (one for each peer) and ensure only the third
// is called.
TEST_F(GattTest, MultipleRegisterRemoteServiceWatcherForPeers) {
  // Configure peer 0 with 1 service.
  const att::Handle kSvcStartHandle0(42);
  const att::Handle kSvcEndHandle0(kSvcStartHandle0);
  ServiceData svc_data_0(ServiceKind::PRIMARY,
                         kSvcStartHandle0,
                         kSvcEndHandle0,
                         kTestServiceUuid0);
  auto client_0 = std::make_unique<testing::FakeClient>(dispatcher());
  client_0->set_services({svc_data_0});

  gatt()->AddConnection(kPeerId0, std::move(client_0), CreateMockServer);

  std::vector<ServiceWatcherData> watcher_data_0;
  GATT::RemoteServiceWatcherId id_0 =
      gatt()->RegisterRemoteServiceWatcherForPeer(
          kPeerId0,
          [&](std::vector<att::Handle> removed,
              ServiceList added,
              ServiceList modified) {
            watcher_data_0.push_back(
                ServiceWatcherData{.peer_id = kPeerId0,
                                   .removed = std::move(removed),
                                   .added = std::move(added),
                                   .modified = std::move(modified)});
          });

  // Configure peer 1 with gatt service.
  const att::Handle kGattSvcStartHandle(1);
  const att::Handle kSvcChangedChrcHandle(2);
  const att::Handle kSvcChangedChrcValueHandle(3);
  const att::Handle kCCCDescriptorHandle(4);
  const att::Handle kGattSvcEndHandle(kCCCDescriptorHandle);
  ServiceData gatt_svc(ServiceKind::PRIMARY,
                       kGattSvcStartHandle,
                       kGattSvcEndHandle,
                       types::kGenericAttributeService);
  CharacteristicData service_changed_chrc(Property::kIndicate,
                                          std::nullopt,
                                          kSvcChangedChrcHandle,
                                          kSvcChangedChrcValueHandle,
                                          types::kServiceChangedCharacteristic);
  DescriptorData ccc_descriptor(kCCCDescriptorHandle,
                                types::kClientCharacteristicConfig);
  auto client_1 = std::make_unique<testing::FakeClient>(dispatcher());
  auto client_1_weak = client_1.get();
  client_1->set_services({gatt_svc});
  client_1->set_characteristics({service_changed_chrc});
  client_1->set_descriptors({ccc_descriptor});

  // Return success when a Service Changed Client Characteristic Config
  // descriptor write is performed.
  client_1->set_write_request_callback(
      [&](att::Handle, const auto&, auto status_callback) {
        status_callback(fit::ok());
      });

  gatt()->AddConnection(kPeerId1, std::move(client_1), CreateMockServer);

  // Register 2 watchers for kPeerId1.
  std::vector<ServiceWatcherData> watcher_data_1;
  GATT::RemoteServiceWatcherId id_1 =
      gatt()->RegisterRemoteServiceWatcherForPeer(
          kPeerId1,
          [&](std::vector<att::Handle> removed,
              ServiceList added,
              ServiceList modified) {
            watcher_data_1.push_back(
                ServiceWatcherData{.peer_id = kPeerId1,
                                   .removed = std::move(removed),
                                   .added = std::move(added),
                                   .modified = std::move(modified)});
          });
  EXPECT_NE(id_0, id_1);

  std::vector<ServiceWatcherData> watcher_data_2;
  gatt()->RegisterRemoteServiceWatcherForPeer(
      kPeerId1,
      [&](std::vector<att::Handle> removed,
          ServiceList added,
          ServiceList modified) {
        watcher_data_2.push_back(
            ServiceWatcherData{.peer_id = kPeerId1,
                               .removed = std::move(removed),
                               .added = std::move(added),
                               .modified = std::move(modified)});
      });

  // Service discovery should complete and all service watchers should be
  // notified.
  gatt()->InitializeClient(kPeerId0, /*service_uuids=*/{});
  gatt()->InitializeClient(kPeerId1, /*service_uuids=*/{});
  RunUntilIdle();
  ASSERT_EQ(watcher_data_0.size(), 1u);
  ASSERT_EQ(watcher_data_0[0].added.size(), 1u);
  EXPECT_EQ(watcher_data_0[0].added[0]->handle(), kSvcStartHandle0);

  ASSERT_EQ(watcher_data_1.size(), 1u);
  ASSERT_EQ(watcher_data_1[0].added.size(), 1u);
  EXPECT_EQ(watcher_data_1[0].added[0]->handle(), kGattSvcStartHandle);

  ASSERT_EQ(watcher_data_2.size(), 1u);
  ASSERT_EQ(watcher_data_2[0].added.size(), 1u);
  EXPECT_EQ(watcher_data_2[0].added[0]->handle(), kGattSvcStartHandle);

  gatt()->UnregisterRemoteServiceWatcher(id_0);
  gatt()->UnregisterRemoteServiceWatcher(id_1);

  const att::Handle kSvcStartHandle1(84);
  const att::Handle kSvcEndHandle1(kSvcStartHandle1);
  ServiceData svc_data_1(ServiceKind::PRIMARY,
                         kSvcStartHandle1,
                         kSvcEndHandle1,
                         kTestServiceUuid0);
  client_1_weak->set_services({gatt_svc, svc_data_1});

  // Send a notification that service kSvcStartHandle1 has been added.
  StaticByteBuffer svc_changed_range_buffer(
      LowerBits(kSvcStartHandle1),
      UpperBits(kSvcStartHandle1),  // start handle of affected range
      LowerBits(kSvcEndHandle1),
      UpperBits(kSvcEndHandle1)  // end handle of affected range
  );
  client_1_weak->SendNotification(/*indicate=*/true,
                                  kSvcChangedChrcValueHandle,
                                  svc_changed_range_buffer,
                                  /*maybe_truncated=*/false);
  RunUntilIdle();
  // Unregistered handlers should not be notified.
  ASSERT_EQ(watcher_data_0.size(), 1u);
  ASSERT_EQ(watcher_data_1.size(), 1u);

  // Still registered handler should be notified of added service.
  ASSERT_EQ(watcher_data_2.size(), 2u);
  ASSERT_EQ(watcher_data_2[1].added.size(), 1u);
  EXPECT_EQ(watcher_data_2[1].added[0]->handle(), kSvcStartHandle1);
}

TEST_F(GattTest, ServiceDiscoveryFailureShutsDownConnection) {
  testing::MockServer::WeakPtr mock_server;
  auto mock_server_factory = [&](PeerId peer_id,
                                 LocalServiceManager::WeakPtr local_services) {
    auto unique_mock_server = std::make_unique<testing::MockServer>(
        peer_id, std::move(local_services));
    mock_server = unique_mock_server->AsMockWeakPtr();
    return unique_mock_server;
  };
  fake_client()->set_discover_services_callback([](ServiceKind) {
    return ToResult(att::ErrorCode::kRequestNotSupported);
  });
  gatt()->AddConnection(kPeerId, take_client(), std::move(mock_server_factory));
  ASSERT_TRUE(mock_server.is_alive());
  EXPECT_FALSE(mock_server->was_shut_down());
  gatt()->InitializeClient(kPeerId, std::vector<UUID>{});
  RunUntilIdle();

#ifndef NINSPECT
  EXPECT_EQ(InspectProfileDiscoveryFailureCount(), 1u);
#endif
  EXPECT_TRUE(mock_server->was_shut_down());
}

#ifndef NINSPECT
TEST_F(GattTest, ServiceDiscoveryFailureIsAggregated) {
  for (size_t i = 0; i < 10; i++) {
    auto client = std::make_unique<testing::FakeClient>(dispatcher());
    client->set_discover_services_callback([](ServiceKind) {
      return ToResult(att::ErrorCode::kRequestNotSupported);
    });
    PeerId peer(i);
    gatt()->AddConnection(peer, std::move(client), CreateMockServer);
    gatt()->InitializeClient(peer, std::vector<UUID>{});
    RunUntilIdle();

    EXPECT_EQ(InspectProfileDiscoveryFailureCount(), i + 1);
  }
}
#endif

TEST_F(GattTest, SendIndicationNoConnectionFails) {
  att::Result<> res = fit::ok();
  auto indicate_cb = [&res](att::Result<> cb_res) { res = cb_res; };
  // Don't add the connection to GATT before trying to send an indication
  gatt()->SendUpdate(/*service_id=*/1,
                     /*chrc_id=*/2,
                     PeerId{3},
                     std::vector<uint8_t>{1},
                     std::move(indicate_cb));
  EXPECT_EQ(fit::failed(), res);
}

class GattTestBoolParam : public GattTest,
                          public ::testing::WithParamInterface<bool> {};

TEST_P(GattTestBoolParam, SendIndicationReceiveResponse) {
  testing::MockServer::WeakPtr mock_server;
  auto mock_server_factory = [&](PeerId peer_id,
                                 LocalServiceManager::WeakPtr local_services) {
    auto unique_mock_server = std::make_unique<testing::MockServer>(
        peer_id, std::move(local_services));
    mock_server = unique_mock_server->AsMockWeakPtr();
    return unique_mock_server;
  };
  gatt()->AddConnection(kPeerId, take_client(), std::move(mock_server_factory));
  ASSERT_TRUE(mock_server.is_alive());

  // Configure how the mock server handles updates sent from the GATT object.
  IndicationCallback mock_ind_cb = nullptr;
  const std::vector<uint8_t> kIndicateVal{114};  // 114 is arbitrary
  testing::UpdateHandler handler = [&](auto /*ignore*/,
                                       auto /*ignore*/,
                                       const ByteBuffer& bytes,
                                       IndicationCallback ind_cb) {
    EXPECT_EQ(kIndicateVal, bytes.ToVector());
    mock_ind_cb = std::move(ind_cb);
  };
  mock_server->set_update_handler(std::move(handler));

  // Registering the service isn't strictly necessary as gatt::GATT itself is
  // not responsible for checking that a service exists before sending an
  // update, but it's a more realistic test.
  IdType svc_id = RegisterArbitraryService();

  std::optional<att::Result<>> indicate_status;
  auto indicate_cb = [&](att::Result<> status) { indicate_status = status; };
  gatt()->SendUpdate(
      svc_id, kChrcId, kPeerId, kIndicateVal, std::move(indicate_cb));
  RunUntilIdle();
  EXPECT_TRUE(mock_ind_cb);
  EXPECT_FALSE(indicate_status.has_value());
  if (GetParam()) {
    mock_ind_cb(fit::ok());
    ASSERT_TRUE(indicate_status.has_value());
    EXPECT_TRUE(indicate_status->is_ok());
  } else {
    mock_ind_cb(ToResult(HostError::kTimedOut));
    ASSERT_TRUE(indicate_status.has_value());
    EXPECT_EQ(ToResult(HostError::kTimedOut), *indicate_status);
  }
}

INSTANTIATE_TEST_SUITE_P(GattTestBoolParamTests,
                         GattTestBoolParam,
                         ::testing::Bool());

TEST_F(GattTest, NotifyConnectedPeersNoneConnectedDoesntCrash) {
  // Registering a service isn't strictly necessary, but makes for a more
  // realistic test.
  IdType svc_id = RegisterArbitraryService();

  const std::vector<uint8_t> kNotifyVal{12u};
  gatt()->UpdateConnectedPeers(
      svc_id, kChrcId, kNotifyVal, /*indicate_cb=*/nullptr);
  RunUntilIdle();
}

TEST_F(GattTest, NotifyConnectedPeerWithConnectionDoesntCrash) {
  // Registering a service isn't strictly necessary, but makes for a more
  // realistic test.
  IdType svc_id = RegisterArbitraryService();

  testing::MockServer::WeakPtr mock_server;
  auto mock_server_factory = [&](PeerId peer_id,
                                 LocalServiceManager::WeakPtr local_services) {
    auto unique_mock_server = std::make_unique<testing::MockServer>(
        peer_id, std::move(local_services));
    mock_server = unique_mock_server->AsMockWeakPtr();
    return unique_mock_server;
  };
  gatt()->AddConnection(kPeerId, take_client(), std::move(mock_server_factory));
  ASSERT_TRUE(mock_server.is_alive());

  // Configure how the mock server handles updates sent from the GATT object.
  IndicationCallback mock_ind_cb = [](att::Result<>) {};  // no-op, but not-null
  const std::vector<uint8_t> kNotifyVal{12u};
  testing::UpdateHandler handler = [&](auto /*ignore*/,
                                       auto /*ignore*/,
                                       const ByteBuffer& bytes,
                                       IndicationCallback ind_cb) {
    EXPECT_EQ(kNotifyVal, bytes.ToVector());
    mock_ind_cb = std::move(ind_cb);
  };
  mock_server->set_update_handler(std::move(handler));
  gatt()->UpdateConnectedPeers(
      svc_id, kChrcId, kNotifyVal, /*indicate_cb=*/nullptr);
  RunUntilIdle();
  EXPECT_EQ(nullptr, mock_ind_cb);
}

TEST_F(GattTest, IndicateConnectedPeersNoneConnectedSucceeds) {
  // Registering a service isn't strictly necessary, but makes for a more
  // realistic test.
  IdType svc_id = RegisterArbitraryService();

  const std::vector<uint8_t> indicate_val{12u};
  att::Result<> res = ToResult(att::ErrorCode::kAttributeNotFound);
  auto indicate_cb = [&](att::Result<> cb_res) { res = cb_res; };
  gatt()->UpdateConnectedPeers(
      svc_id, kChrcId, indicate_val, std::move(indicate_cb));

  EXPECT_EQ(fit::ok(), res);
}

struct PeerIdAndMtu {
  PeerId peer_id;
  uint16_t mtu;
};
TEST_F(GattTest, UpdateMtuListenersNotified) {
  // Add MTU listeners to GATT
  std::optional<PeerIdAndMtu> listener_1_results;
  auto listener_1 = [&](PeerId peer_id, uint16_t mtu) {
    listener_1_results = PeerIdAndMtu{.peer_id = peer_id, .mtu = mtu};
  };
  std::optional<PeerIdAndMtu> listener_2_results;
  auto listener_2 = [&](PeerId peer_id, uint16_t mtu) {
    listener_2_results = PeerIdAndMtu{.peer_id = peer_id, .mtu = mtu};
  };
  gatt()->RegisterPeerMtuListener(std::move(listener_1));
  GATT::PeerMtuListenerId listener_2_id =
      gatt()->RegisterPeerMtuListener(std::move(listener_2));

  // Configure MTU
  const uint16_t kExpectedMtu = att::kLEMinMTU + 1;
  fake_client()->set_server_mtu(kExpectedMtu);

  // Add connection, initialize, and verify that MTU exchange succeeds
  gatt()->AddConnection(kPeerId0, take_client(), CreateMockServer);
  gatt()->InitializeClient(kPeerId0, {});
  RunUntilIdle();

  ASSERT_TRUE(listener_1_results.has_value());
  EXPECT_EQ(kPeerId0, listener_1_results->peer_id);
  EXPECT_EQ(kExpectedMtu, listener_1_results->mtu);
  ASSERT_TRUE(listener_2_results.has_value());
  EXPECT_EQ(kPeerId0, listener_2_results->peer_id);
  EXPECT_EQ(kExpectedMtu, listener_2_results->mtu);

  // After unregistering listener_2, only listener_1 should be notified for the
  // next update
  listener_1_results.reset();
  listener_2_results.reset();
  EXPECT_TRUE(gatt()->UnregisterPeerMtuListener(listener_2_id));
  auto client_2 = std::make_unique<testing::FakeClient>(dispatcher());
  const uint16_t kNewExpectedMtu = kExpectedMtu + 1;
  client_2->set_server_mtu(kNewExpectedMtu);
  gatt()->AddConnection(kPeerId1, std::move(client_2), CreateMockServer);
  gatt()->InitializeClient(kPeerId1, {});
  RunUntilIdle();

  ASSERT_TRUE(listener_1_results.has_value());
  EXPECT_EQ(kPeerId1, listener_1_results->peer_id);
  EXPECT_EQ(kNewExpectedMtu, listener_1_results->mtu);
  EXPECT_FALSE(listener_2_results.has_value());
}

TEST_F(GattTest, MtuExchangeServerNotSupportedListenersNotifiedDefaultMtu) {
  // Add MTU listeners to GATT
  std::optional<PeerIdAndMtu> listener_1_results;
  auto listener_1 = [&](PeerId peer_id, uint16_t mtu) {
    listener_1_results = PeerIdAndMtu{.peer_id = peer_id, .mtu = mtu};
  };
  std::optional<PeerIdAndMtu> listener_2_results;
  auto listener_2 = [&](PeerId peer_id, uint16_t mtu) {
    listener_2_results = PeerIdAndMtu{.peer_id = peer_id, .mtu = mtu};
  };
  gatt()->RegisterPeerMtuListener(std::move(listener_1));
  gatt()->RegisterPeerMtuListener(std::move(listener_2));

  // It should be OK for the MTU exchange to fail with kRequestNotSupported, in
  // which case we use the default LE MTU per v5.3 Vol. 3 Part G 4.3.1 (not the
  // Server MTU)
  fake_client()->set_server_mtu(att::kLEMinMTU + 5);
  fake_client()->set_exchange_mtu_status(
      ToResult(att::ErrorCode::kRequestNotSupported));
  gatt()->AddConnection(kPeerId0, take_client(), CreateMockServer);
  gatt()->InitializeClient(kPeerId0, {});
  RunUntilIdle();

  ASSERT_TRUE(listener_1_results.has_value());
  EXPECT_EQ(kPeerId0, listener_1_results->peer_id);
  EXPECT_EQ(att::kLEMinMTU, listener_1_results->mtu);
  ASSERT_TRUE(listener_2_results.has_value());
  EXPECT_EQ(kPeerId0, listener_2_results->peer_id);
  EXPECT_EQ(att::kLEMinMTU, listener_2_results->mtu);
}

TEST_F(GattTest, MtuExchangeFailsListenersNotNotifiedConnectionShutdown) {
  // Add MTU listener to GATT
  bool listener_invoked = false;
  auto listener = [&](PeerId /*ignore*/, uint16_t /*ignore*/) {
    listener_invoked = true;
  };
  gatt()->RegisterPeerMtuListener(std::move(listener));

  // Configure MTU exchange to fail
  fake_client()->set_exchange_mtu_status(ToResult(HostError::kFailed));

  // Track mock server
  testing::MockServer::WeakPtr mock_server;
  auto mock_server_factory = [&](PeerId peer_id,
                                 LocalServiceManager::WeakPtr local_services) {
    auto unique_mock_server = std::make_unique<testing::MockServer>(
        peer_id, std::move(local_services));
    mock_server = unique_mock_server->AsMockWeakPtr();
    return unique_mock_server;
  };

  // Add connection, initialize, and verify that MTU exchange failure causes
  // connection shutdown.
  gatt()->AddConnection(
      kPeerId0, take_client(), std::move(mock_server_factory));
  gatt()->InitializeClient(kPeerId0, {});
  RunUntilIdle();
  EXPECT_FALSE(listener_invoked);
  ASSERT_TRUE(mock_server.is_alive());
  EXPECT_TRUE(mock_server->was_shut_down());
}

const std::vector<uint8_t> kIndicateVal{12u};
class GattIndicateMultipleConnectedPeersTest : public GattTest {
 protected:
  void SetUp() override {
    GattTest::SetUp();
    // Registering a service isn't strictly necessary, but makes for a more
    // realistic test.
    svc_id_ = RegisterArbitraryService();

    // Add first connection
    auto mock_server_factory_0 =
        [&](PeerId peer_id, LocalServiceManager::WeakPtr local_services) {
          auto unique_mock_server = std::make_unique<testing::MockServer>(
              peer_id, std::move(local_services));
          mock_server_0_ = unique_mock_server->AsMockWeakPtr();
          return unique_mock_server;
        };
    gatt()->AddConnection(kPeerId0,
                          std::make_unique<testing::FakeClient>(dispatcher()),
                          std::move(mock_server_factory_0));
    ASSERT_TRUE(mock_server_0_.is_alive());

    // Add second connection
    auto mock_server_factory_1 =
        [&](PeerId peer_id, LocalServiceManager::WeakPtr local_services) {
          auto unique_mock_server = std::make_unique<testing::MockServer>(
              peer_id, std::move(local_services));
          mock_server_1_ = unique_mock_server->AsMockWeakPtr();
          return unique_mock_server;
        };
    gatt()->AddConnection(kPeerId1,
                          std::make_unique<testing::FakeClient>(dispatcher()),
                          std::move(mock_server_factory_1));
    ASSERT_TRUE(mock_server_1_.is_alive());

    // Configure how the mock servers handle updates from the GATT object.
    testing::UpdateHandler handler_0 = [&](auto /*ignore*/,
                                           auto /*ignore*/,
                                           const ByteBuffer& bytes,
                                           IndicationCallback ind_cb) {
      EXPECT_EQ(kIndicateVal, bytes.ToVector());
      indication_ack_cb_0_ = std::move(ind_cb);
    };
    mock_server_0_->set_update_handler(std::move(handler_0));

    testing::UpdateHandler handler_1 = [&](auto /*ignore*/,
                                           auto /*ignore*/,
                                           const ByteBuffer& bytes,
                                           IndicationCallback ind_cb) {
      EXPECT_EQ(kIndicateVal, bytes.ToVector());
      indication_ack_cb_1_ = std::move(ind_cb);
    };
    mock_server_1_->set_update_handler(std::move(handler_1));
  }

  IndicationCallback indication_ack_cb_0_;
  IndicationCallback indication_ack_cb_1_;
  IdType svc_id_;
  testing::MockServer::WeakPtr mock_server_0_;
  testing::MockServer::WeakPtr mock_server_1_;
};

TEST_F(GattIndicateMultipleConnectedPeersTest,
       UpdateConnectedPeersWaitsTillAllCallbacksComplete) {
  // Send an indication.
  att::Result<> res =
      ToResult(att::ErrorCode::kInvalidPDU);  // arbitrary error code
  IndicationCallback indication_cb = [&res](att::Result<> cb_res) {
    res = cb_res;
  };
  gatt()->UpdateConnectedPeers(
      svc_id_, kChrcId, kIndicateVal, indication_cb.share());
  RunUntilIdle();
  ASSERT_TRUE(indication_ack_cb_0_);
  ASSERT_TRUE(indication_ack_cb_1_);

  // The UpdateConnectedPeers callback shouldn't resolved when the first
  // indication is ACKed.
  indication_ack_cb_0_(fit::ok());
  RunUntilIdle();
  EXPECT_EQ(ToResult(att::ErrorCode::kInvalidPDU), res);

  indication_ack_cb_1_(fit::ok());
  RunUntilIdle();
  EXPECT_EQ(fit::ok(), res);
}

TEST_F(GattIndicateMultipleConnectedPeersTest,
       OneFailsNextSucceedsOnlyFailureNotified) {
  std::optional<att::Result<>> res;
  IndicationCallback indication_cb = [&res](att::Result<> cb_res) {
    res = cb_res;
  };
  gatt()->UpdateConnectedPeers(
      svc_id_, kChrcId, kIndicateVal, indication_cb.share());
  RunUntilIdle();
  EXPECT_EQ(std::nullopt, res);
  ASSERT_TRUE(indication_ack_cb_0_);
  ASSERT_TRUE(indication_ack_cb_1_);

  indication_ack_cb_0_(ToResult(att::ErrorCode::kRequestNotSupported));
  EXPECT_EQ(ToResult(att::ErrorCode::kRequestNotSupported), res);

  // Acking the next indication should not cause the callback to be invoked
  // again with success.
  indication_ack_cb_1_(fit::ok());
  EXPECT_EQ(ToResult(att::ErrorCode::kRequestNotSupported), res);
}

}  // namespace
}  // namespace bt::gatt::internal
