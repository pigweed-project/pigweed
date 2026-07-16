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

#include "pw_bluetooth_sapphire/internal/host/gatt/remote_service_manager.h"

#include <gmock/gmock.h>
#include <pw_assert/check.h>

#include <vector>

#include "pw_async/fake_dispatcher_fixture.h"
#include "pw_bluetooth_sapphire/internal/host/att/att.h"
#include "pw_bluetooth_sapphire/internal/host/att/error.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/fake_client.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/gatt_defs.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"

namespace bt::gatt::internal {
namespace {

using namespace ::testing;

constexpr UUID kTestServiceUuid1(uint16_t{0xbeef});
constexpr UUID kTestServiceUuid2(uint16_t{0xcafe});
constexpr UUID kTestServiceUuid3(uint16_t{0xface});
constexpr UUID kTestUuid3(uint16_t{0xfefe});
constexpr UUID kTestUuid4(uint16_t{0xefef});

// Buffers for descriptor responses.
// ExtendedProperty::kReliableWrite enabled.
const StaticByteBuffer kExtendedPropValue(0x01, 0x00);
const StaticByteBuffer kCCCNotifyValue(0x01, 0x00);
const StaticByteBuffer kCCCIndicateValue(0x02, 0x00);

// Constants used for initializing fake characteristic data.
constexpr att::Handle kStart = 1;
constexpr att::Handle kCharDecl = 2;
constexpr att::Handle kCharValue = 3;
constexpr att::Handle kDesc1 = 4;
constexpr att::Handle kDesc2 = 5;
constexpr att::Handle kEnd = 5;

void NopStatusCallback(att::Result<>) {}
void NopMtuCallback(uint16_t) {}
void NopValueCallback(const ByteBuffer& /*value*/, bool /*maybe_truncated*/) {}

class RemoteServiceManagerTest : public pw::async::test::FakeDispatcherFixture {
 public:
  RemoteServiceManagerTest() = default;
  ~RemoteServiceManagerTest() override = default;

 protected:
  void SetUp() override {
    auto client = std::make_unique<testing::FakeClient>(dispatcher());
    fake_client_ = client.get();

    mgr_ = std::make_unique<RemoteServiceManager>(std::move(client));
  }

  void TearDown() override {
    // Clear any previous expectations that are based on the ATT Write Request,
    // so that write requests sent during RemoteService::ShutDown() are ignored.
    if (fake_client_) {
      fake_client()->set_write_request_callback({});
    }
    mgr_ = nullptr;
  }

  // Initializes a RemoteService based on |data|.
  RemoteService::WeakPtr SetUpFakeService(const ServiceData& data) {
    std::vector<ServiceData> fake_services{{data}};
    fake_client()->set_services(std::move(fake_services));

    mgr()->Initialize(NopStatusCallback, NopMtuCallback);

    ServiceList services;
    mgr()->ListServices(std::vector<UUID>(),
                        [&services](auto, ServiceList cb_services) {
                          services = std::move(cb_services);
                        });

    RunUntilIdle();

    PW_DCHECK(services.size() == 1u);
    return services[0];
  }

  void SetCharacteristicsAndDescriptors(
      std::vector<CharacteristicData> fake_chrs,
      std::vector<DescriptorData> fake_descrs = std::vector<DescriptorData>()) {
    fake_client()->set_characteristics(std::move(fake_chrs));
    fake_client()->set_descriptors(std::move(fake_descrs));
    fake_client()->set_characteristic_discovery_status(fit::ok());
  }

  // Discover the characteristics of |service| based on the given |fake_data|.
  void SetupCharacteristics(
      RemoteService::WeakPtr service,
      std::vector<CharacteristicData> fake_chrs,
      std::vector<DescriptorData> fake_descrs = std::vector<DescriptorData>()) {
    PW_DCHECK(service.is_alive());

    SetCharacteristicsAndDescriptors(std::move(fake_chrs),
                                     std::move(fake_descrs));

    service->DiscoverCharacteristics([](auto, const auto&) {});
    RunUntilIdle();
  }

  RemoteService::WeakPtr SetupServiceWithChrcs(
      const ServiceData& data,
      std::vector<CharacteristicData> fake_chrs,
      std::vector<DescriptorData> fake_descrs = std::vector<DescriptorData>()) {
    auto service = SetUpFakeService(data);
    SetupCharacteristics(service, fake_chrs, fake_descrs);
    return service;
  }

  // Create a fake service with one notifiable characteristic.
  RemoteService::WeakPtr SetupNotifiableService() {
    ServiceData data(ServiceKind::PRIMARY, 1, 4, kTestServiceUuid1);
    auto service = SetUpFakeService(data);

    CharacteristicData chr(Property::kNotify, std::nullopt, 2, 3, kTestUuid3);
    DescriptorData desc(4, types::kClientCharacteristicConfig);
    SetupCharacteristics(service, {{chr}}, {{desc}});

    fake_client()->set_write_request_callback(
        [&](att::Handle, const auto&, auto status_callback) {
          status_callback(fit::ok());
        });

    RunUntilIdle();

    return service;
  }

  void EnableNotifications(
      RemoteService::WeakPtr service,
      CharacteristicHandle chr_id,
      att::Result<>* out_status,
      IdType* out_id,
      RemoteService::ValueCallback callback = NopValueCallback) {
    PW_DCHECK(out_status);
    PW_DCHECK(out_id);
    service->EnableNotifications(chr_id,
                                 std::move(callback),
                                 [&](att::Result<> cb_status, IdType cb_id) {
                                   *out_status = cb_status;
                                   *out_id = cb_id;
                                 });
    RunUntilIdle();
  }

  void DestroyServiceManager() {
    mgr_.reset();
    fake_client_ = nullptr;
  }

  RemoteServiceManager* mgr() const { return mgr_.get(); }
  testing::FakeClient* fake_client() const { return fake_client_; }

 private:
  std::unique_ptr<RemoteServiceManager> mgr_;

  // The memory is owned by |mgr_|.
  testing::FakeClient* fake_client_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(RemoteServiceManagerTest);
};

TEST_F(RemoteServiceManagerTest, InitializeNoServices) {
  ServiceList services;
  mgr()->set_service_watcher(
      [&services](auto /*removed*/, ServiceList added, auto /*modified*/) {
        services.insert(services.end(), added.begin(), added.end());
      });

  att::Result<> status = ToResult(HostError::kFailed);
  mgr()->Initialize([&status](att::Result<> val) { status = val; },
                    NopMtuCallback);

  RunUntilIdle();

  EXPECT_EQ(fit::ok(), status);
  EXPECT_TRUE(services.empty());

  mgr()->ListServices(std::vector<UUID>(),
                      [&services](auto, ServiceList cb_services) {
                        services = std::move(cb_services);
                      });
  EXPECT_TRUE(services.empty());
}

TEST_F(RemoteServiceManagerTest, Initialize) {
  ServiceData svc1(ServiceKind::PRIMARY, 1, 1, kTestServiceUuid1);
  ServiceData svc2(ServiceKind::PRIMARY, 2, 2, kTestServiceUuid2);
  std::vector<ServiceData> fake_services{{svc1, svc2}};
  fake_client()->set_services(std::move(fake_services));

  ServiceList services;
  mgr()->set_service_watcher(
      [&services](auto /*removed*/, ServiceList added, auto /*modified*/) {
        services.insert(services.end(), added.begin(), added.end());
      });

  const uint16_t kArbitraryMtu = att::kLEMinMTU + 10;
  fake_client()->set_server_mtu(kArbitraryMtu);

  uint16_t new_mtu = 0;
  auto mtu_cb = [&](uint16_t cb_mtu) { new_mtu = cb_mtu; };

  att::Result<> status = ToResult(HostError::kFailed);
  mgr()->Initialize([&status](att::Result<> val) { status = val; },
                    std::move(mtu_cb));

  RunUntilIdle();

  EXPECT_EQ(kArbitraryMtu, new_mtu);

  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(2u, services.size());
  EXPECT_EQ(svc1.range_start, services[0]->handle());
  EXPECT_EQ(svc2.range_start, services[1]->handle());
  EXPECT_EQ(svc1.type, services[0]->uuid());
  EXPECT_EQ(svc2.type, services[1]->uuid());
}

TEST_F(RemoteServiceManagerTest, InitializeFailure) {
  fake_client()->set_discover_services_callback([](ServiceKind kind) {
    if (kind == ServiceKind::PRIMARY) {
      return ToResult(att::ErrorCode::kRequestNotSupported);
    }
    return att::Result<>(fit::ok());
  });

  ServiceList watcher_services;
  mgr()->set_service_watcher([&watcher_services](auto /*removed*/,
                                                 ServiceList added,
                                                 auto /*modified*/) {
    watcher_services.insert(watcher_services.end(), added.begin(), added.end());
  });

  ServiceList services;
  mgr()->ListServices(std::vector<UUID>(),
                      [&services](auto, ServiceList cb_services) {
                        services = std::move(cb_services);
                      });
  ASSERT_TRUE(services.empty());

  att::Result<> status = ToResult(HostError::kFailed);
  mgr()->Initialize([&status](att::Result<> val) { status = val; },
                    NopMtuCallback);

  RunUntilIdle();

  EXPECT_EQ(ToResult(att::ErrorCode::kRequestNotSupported), status);
  EXPECT_TRUE(services.empty());
  EXPECT_TRUE(watcher_services.empty());
}

TEST_F(RemoteServiceManagerTest, InitializeMtuExchangeNotSupportedSucceeds) {
  ServiceData svc(ServiceKind::PRIMARY, 1, 1, kTestServiceUuid1);
  fake_client()->set_services({svc});
  // The MTU exchange is an optional procedure, so if the peer tells us that
  // they do not support it, we should continue with initialization.
  fake_client()->set_exchange_mtu_status(
      ToResult(att::ErrorCode::kRequestNotSupported));

  ServiceList services;
  mgr()->set_service_watcher(
      [&services](auto /*removed*/, ServiceList added, auto /*modified*/) {
        services.insert(services.end(), added.begin(), added.end());
      });

  std::optional<uint16_t> mtu;
  auto mtu_cb = [&mtu](uint16_t cb_mtu) { mtu = cb_mtu; };

  att::Result<> status = ToResult(HostError::kFailed);
  mgr()->Initialize([&status](att::Result<> val) { status = val; },
                    std::move(mtu_cb));

  RunUntilIdle();

  EXPECT_EQ(fit::ok(), status);
  ASSERT_EQ(1u, services.size());
  EXPECT_EQ(svc.range_start, services[0]->handle());
  EXPECT_EQ(svc.type, services[0]->uuid());

  // If the MTU exchange isn't supported, "the default MTU shall be used" (v5.3
  // Vol. 3 Part G 4.3.1)
  ASSERT_TRUE(mtu.has_value());
  EXPECT_EQ(att::kLEMinMTU, *mtu);
}

TEST_F(RemoteServiceManagerTest, InitializeMtuExchangeFailure) {
  fake_client()->set_exchange_mtu_status(
      ToResult(att::ErrorCode::kUnlikelyError));

  bool mtu_updated = false;
  auto mtu_cb = [&](uint16_t /*ignore*/) { mtu_updated = true; };

  att::Result<> status = fit::ok();
  mgr()->Initialize([&status](att::Result<> val) { status = val; },
                    std::move(mtu_cb));

  RunUntilIdle();

  EXPECT_EQ(ToResult(att::ErrorCode::kUnlikelyError), status);
  EXPECT_FALSE(mtu_updated);
}

TEST_F(RemoteServiceManagerTest, InitializeByUUIDNoServices) {
  ServiceList services;
  mgr()->set_service_watcher(
      [&services](auto /*removed*/, ServiceList added, auto /*modified*/) {
        services.insert(services.end(), added.begin(), added.end());
      });

  att::Result<> status = ToResult(HostError::kFailed);
  mgr()->Initialize([&status](att::Result<> val) { status = val; },
                    NopMtuCallback,
                    {kTestServiceUuid1});

  RunUntilIdle();

  EXPECT_EQ(fit::ok(), status);
  EXPECT_TRUE(services.empty());

  mgr()->ListServices(std::vector<UUID>(),
                      [&services](auto, ServiceList cb_services) {
                        services = std::move(cb_services);
                      });
  EXPECT_TRUE(services.empty());
}

TEST_F(RemoteServiceManagerTest, InitializeWithUuids) {
  ServiceData svc1(ServiceKind::PRIMARY, 1, 1, kTestServiceUuid1);
  ServiceData svc2(ServiceKind::PRIMARY, 2, 2, kTestServiceUuid2);
  ServiceData svc3(ServiceKind::PRIMARY, 3, 3, kTestServiceUuid3);
  std::vector<ServiceData> fake_services{{svc1, svc2, svc3}};
  fake_client()->set_services(std::move(fake_services));

  ServiceList services;
  mgr()->set_service_watcher(
      [&services](auto /*removed*/, ServiceList added, auto /*modified*/) {
        services.insert(services.end(), added.begin(), added.end());
      });

  att::Result<> status = ToResult(HostError::kFailed);
  mgr()->Initialize([&status](att::Result<> val) { status = val; },
                    NopMtuCallback,
                    {kTestServiceUuid1, kTestServiceUuid3});

  RunUntilIdle();

  EXPECT_EQ(fit::ok(), status);
  std::vector<ServiceData> found_services;
  for (const auto& ptr : services) {
    found_services.push_back(ptr->info());
  }
  EXPECT_THAT(found_services, UnorderedElementsAre(Eq(svc1), Eq(svc3)));
}

TEST_F(RemoteServiceManagerTest, InitializeByUUIDFailure) {
  fake_client()->set_discover_services_callback([](ServiceKind kind) {
    if (kind == ServiceKind::PRIMARY) {
      return ToResult(att::ErrorCode::kRequestNotSupported);
    }
    return att::Result<>(fit::ok());
  });

  ServiceList watcher_services;
  mgr()->set_service_watcher([&watcher_services](auto /*removed*/,
                                                 ServiceList added,
                                                 auto /*modified*/) {
    watcher_services.insert(watcher_services.end(), added.begin(), added.end());
  });

  ServiceList services;
  mgr()->ListServices(std::vector<UUID>(),
                      [&services](auto, ServiceList cb_services) {
                        services = std::move(cb_services);
                      });
  ASSERT_TRUE(services.empty());

  att::Result<> status = ToResult(HostError::kFailed);
  mgr()->Initialize([&status](att::Result<> val) { status = val; },
                    NopMtuCallback,
                    {kTestServiceUuid1});

  RunUntilIdle();

  EXPECT_EQ(ToResult(att::ErrorCode::kRequestNotSupported), status);
  EXPECT_TRUE(services.empty());
  EXPECT_TRUE(watcher_services.empty());
}

TEST_F(RemoteServiceManagerTest, InitializeSecondaryServices) {
  ServiceData svc(ServiceKind::SECONDARY, 1, 1, kTestServiceUuid1);
  std::vector<ServiceData> fake_services{{svc}};
  fake_client()->set_services(std::move(fake_services));

  ServiceList services;
  mgr()->set_service_watcher(
      [&services](auto /*removed*/, ServiceList added, auto /*modified*/) {
        services.insert(services.end(), added.begin(), added.end());
      });

  att::Result<> status = ToResult(HostError::kFailed);
  mgr()->Initialize([&status](att::Result<> val) { status = val; },
                    NopMtuCallback);

  RunUntilIdle();

  EXPECT_EQ(fit::ok(), status);
  ASSERT_EQ(1u, services.size());
  EXPECT_EQ(svc.range_start, services[0]->handle());
  EXPECT_EQ(svc.type, services[0]->uuid());
  EXPECT_EQ(ServiceKind::SECONDARY, services[0]->info().kind);
}

TEST_F(RemoteServiceManagerTest, InitializePrimaryAndSecondaryServices) {
  ServiceData svc1(ServiceKind::PRIMARY, 1, 1, kTestServiceUuid1);
  ServiceData svc2(ServiceKind::SECONDARY, 2, 2, kTestServiceUuid2);
  std::vector<ServiceData> fake_services{{svc1, svc2}};
  fake_client()->set_services(std::move(fake_services));

  ServiceList services;
  mgr()->set_service_watcher(
      [&services](auto /*removed*/, ServiceList added, auto /*modified*/) {
        services.insert(services.end(), added.begin(), added.end());
      });

  att::Result<> status = ToResult(HostError::kFailed);
  mgr()->Initialize([&status](att::Result<> val) { status = val; },
                    NopMtuCallback);

  RunUntilIdle();

  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(2u, services.size());
  EXPECT_EQ(ServiceKind::PRIMARY, services[0]->info().kind);
  EXPECT_EQ(ServiceKind::SECONDARY, services[1]->info().kind);
}

TEST_F(RemoteServiceManagerTest,
       InitializePrimaryAndSecondaryServicesOutOfOrder) {
  // RemoteServiceManager discovers primary services first, followed by
  // secondary services. Test that the results are stored and represented in the
  // correct order when a secondary service precedes a primary service.
  ServiceData svc1(ServiceKind::SECONDARY, 1, 1, kTestServiceUuid1);
  ServiceData svc2(ServiceKind::PRIMARY, 2, 2, kTestServiceUuid2);
  fake_client()->set_services({{svc1, svc2}});

  ServiceList services;
  mgr()->set_service_watcher(
      [&services](auto /*removed*/, ServiceList added, auto /*modified*/) {
        services.insert(services.end(), added.begin(), added.end());
      });

  att::Result<> status = ToResult(HostError::kFailed);
  mgr()->Initialize([&status](att::Result<> val) { status = val; },
                    NopMtuCallback);
  RunUntilIdle();

  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(2u, services.size());
  EXPECT_EQ(ServiceKind::SECONDARY, services[0]->info().kind);
  EXPECT_EQ(ServiceKind::PRIMARY, services[1]->info().kind);
}

// Tests that an ATT error that occurs during secondary service aborts
// initialization.
TEST_F(RemoteServiceManagerTest, InitializeSecondaryServicesFailure) {
  fake_client()->set_discover_services_callback([](ServiceKind kind) {
    if (kind == ServiceKind::SECONDARY) {
      return ToResult(att::ErrorCode::kRequestNotSupported);
    }
    return att::Result<>(fit::ok());
  });

  ServiceList services;
  mgr()->set_service_watcher(
      [&services](auto /*removed*/, ServiceList added, auto /*modified*/) {
        services.insert(services.end(), added.begin(), added.end());
      });

  att::Result<> status = fit::ok();
  mgr()->Initialize([&status](att::Result<> val) { status = val; },
                    NopMtuCallback);
  RunUntilIdle();

  EXPECT_EQ(ToResult(att::ErrorCode::kRequestNotSupported), status);
  EXPECT_TRUE(services.empty());
}

// Tests that the "unsupported group type" error is treated as a failure for
// primary services.
TEST_F(RemoteServiceManagerTest,
       InitializePrimaryServicesErrorUnsupportedGroupType) {
  fake_client()->set_discover_services_callback([](ServiceKind kind) {
    if (kind == ServiceKind::PRIMARY) {
      return ToResult(att::ErrorCode::kUnsupportedGroupType);
    }
    return att::Result<>(fit::ok());
  });

  ServiceList services;
  mgr()->set_service_watcher(
      [&services](auto /*removed*/, ServiceList added, auto /*modified*/) {
        services.insert(services.end(), added.begin(), added.end());
      });

  att::Result<> status = fit::ok();
  mgr()->Initialize([&status](att::Result<> val) { status = val; },
                    NopMtuCallback);
  RunUntilIdle();

  EXPECT_EQ(ToResult(att::ErrorCode::kUnsupportedGroupType), status);
  EXPECT_TRUE(services.empty());
}

// Tests that the "unsupported group type" error is NOT treated as a failure for
// secondary services.
TEST_F(RemoteServiceManagerTest,
       InitializeSecondaryServicesErrorUnsupportedGroupTypeIsIgnored) {
  ServiceData svc1(ServiceKind::PRIMARY, 1, 1, kTestServiceUuid1);
  fake_client()->set_services({{svc1}});
  fake_client()->set_discover_services_callback([](ServiceKind kind) {
    if (kind == ServiceKind::SECONDARY) {
      return ToResult(att::ErrorCode::kUnsupportedGroupType);
    }
    return att::Result<>(fit::ok());
  });

  ServiceList services;
  mgr()->set_service_watcher(
      [&services](auto /*removed*/, ServiceList added, auto /*modified*/) {
        services.insert(services.end(), added.begin(), added.end());
      });

  att::Result<> status = fit::ok();
  mgr()->Initialize([&status](att::Result<> val) { status = val; },
                    NopMtuCallback);
  RunUntilIdle();

  EXPECT_EQ(fit::ok(), status);
  ASSERT_EQ(1u, services.size());
  EXPECT_EQ(svc1, services[0]->info());
}

TEST_F(RemoteServiceManagerTest, ListServicesBeforeInit) {
  ServiceData svc(ServiceKind::PRIMARY, 1, 1, kTestServiceUuid1);
  std::vector<ServiceData> fake_services{{svc}};
  fake_client()->set_services(std::move(fake_services));

  ServiceList services;
  mgr()->ListServices(std::vector<UUID>(),
                      [&services](auto, ServiceList cb_services) {
                        services = std::move(cb_services);
                      });
  EXPECT_TRUE(services.empty());

  att::Result<> status = ToResult(HostError::kFailed);
  mgr()->Initialize([&status](att::Result<> val) { status = val; },
                    NopMtuCallback);

  RunUntilIdle();

  EXPECT_EQ(fit::ok(), status);
  ASSERT_EQ(1u, services.size());
  EXPECT_EQ(svc.range_start, services[0]->handle());
  EXPECT_EQ(svc.type, services[0]->uuid());
}

TEST_F(RemoteServiceManagerTest, ListServicesAfterInit) {
  ServiceData svc(ServiceKind::PRIMARY, 1, 1, kTestServiceUuid1);
  std::vector<ServiceData> fake_services{{svc}};
  fake_client()->set_services(std::move(fake_services));

  att::Result<> status = ToResult(HostError::kFailed);
  mgr()->Initialize([&status](att::Result<> val) { status = val; },
                    NopMtuCallback);

  RunUntilIdle();

  ASSERT_EQ(fit::ok(), status);

  ServiceList services;
  mgr()->ListServices(std::vector<UUID>(),
                      [&services](auto, ServiceList cb_services) {
                        services = std::move(cb_services);
                      });
  ASSERT_EQ(1u, services.size());
  EXPECT_EQ(svc.range_start, services[0]->handle());
  EXPECT_EQ(svc.type, services[0]->uuid());
}

TEST_F(RemoteServiceManagerTest, ListServicesByUuid) {
  std::vector<UUID> uuids{kTestServiceUuid1};

  ServiceData svc1(ServiceKind::PRIMARY, 1, 1, kTestServiceUuid1);
  ServiceData svc2(ServiceKind::PRIMARY, 2, 2, kTestServiceUuid2);
  std::vector<ServiceData> fake_services{{svc1, svc2}};
  fake_client()->set_services(std::move(fake_services));

  ServiceList service_watcher_services;
  mgr()->set_service_watcher([&service_watcher_services](auto /*removed*/,
                                                         ServiceList added,
                                                         auto /*modified*/) {
    service_watcher_services.insert(
        service_watcher_services.end(), added.begin(), added.end());
  });

  att::Result<> list_services_status = fit::ok();
  ServiceList list_services;
  mgr()->ListServices(std::move(uuids),
                      [&](att::Result<> cb_status, ServiceList cb_services) {
                        list_services_status = cb_status;
                        list_services = std::move(cb_services);
                      });
  ASSERT_TRUE(service_watcher_services.empty());

  att::Result<> status = ToResult(HostError::kFailed);
  mgr()->Initialize([&status](att::Result<> val) { status = val; },
                    NopMtuCallback);

  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(fit::ok(), list_services_status);
  // Only svc1 has a type in |uuids|.
  EXPECT_EQ(1u, list_services.size());
  EXPECT_EQ(svc1.range_start, list_services[0]->handle());
  EXPECT_EQ(svc1.type, list_services[0]->uuid());
  // All services should be discovered and returned to service watcher because
  // Initialize() was not called with a list of uuids to discover.
  EXPECT_EQ(2u, service_watcher_services.size());
}

TEST_F(RemoteServiceManagerTest, DiscoverCharacteristicsSuccess) {
  auto data = ServiceData(ServiceKind::PRIMARY, 1, 5, kTestServiceUuid1);
  auto service = SetUpFakeService(data);

  CharacteristicData fake_chrc1(0, std::nullopt, 2, 3, kTestUuid3);
  CharacteristicData fake_chrc2(0, std::nullopt, 4, 5, kTestUuid4);
  std::vector<CharacteristicData> fake_chrcs{{fake_chrc1, fake_chrc2}};
  fake_client()->set_characteristics(std::move(fake_chrcs));

  std::map<
      CharacteristicHandle,
      std::pair<CharacteristicData, std::map<DescriptorHandle, DescriptorData>>>
      expected = {{CharacteristicHandle(3), {fake_chrc1, {}}},
                  {CharacteristicHandle(5), {fake_chrc2, {}}}};

  att::Result<> status1 = ToResult(HostError::kFailed);

  auto cb = [expected](att::Result<>* status) {
    return [status, expected](att::Result<> cb_status, const auto& chrcs) {
      *status = cb_status;
      EXPECT_EQ(expected, chrcs);
    };
  };

  service->DiscoverCharacteristics(
      [&](att::Result<> cb_status, const auto& chrcs) {
        status1 = cb_status;
        EXPECT_EQ(expected, chrcs);
      });

  // Queue a second request.
  att::Result<> status2 = ToResult(HostError::kFailed);
  service->DiscoverCharacteristics(
      [&](att::Result<> cb_status, const auto& chrcs) {
        status2 = cb_status;
        EXPECT_EQ(expected, chrcs);
      });

  RunUntilIdle();
  // Only one ATT request should have been made.
  EXPECT_EQ(1u, fake_client()->chrc_discovery_count());
  EXPECT_TRUE(service->IsDiscovered());
  EXPECT_EQ(fit::ok(), status1);
  EXPECT_EQ(fit::ok(), status2);
  EXPECT_EQ(data.range_start,
            fake_client()->last_chrc_discovery_start_handle());
  EXPECT_EQ(data.range_end, fake_client()->last_chrc_discovery_end_handle());

  // Request discovery again. This should succeed without an ATT request.
  status1 = ToResult(HostError::kFailed);
  service->DiscoverCharacteristics(
      [&status1](att::Result<> cb_status, const auto&) {
        status1 = cb_status;
      });

  RunUntilIdle();

  EXPECT_EQ(fit::ok(), status1);
  EXPECT_EQ(1u, fake_client()->chrc_discovery_count());
  EXPECT_TRUE(service->IsDiscovered());
}

TEST_F(RemoteServiceManagerTest, DiscoverCharacteristicsError) {
  auto service = SetUpFakeService(
      ServiceData(ServiceKind::PRIMARY, 1, 5, kTestServiceUuid1));

  CharacteristicData chrc1(0, std::nullopt, 2, 3, kTestUuid3);
  CharacteristicData chrc2(0, std::nullopt, 4, 5, kTestUuid4);
  std::vector<CharacteristicData> fake_chrcs{{chrc1, chrc2}};
  fake_client()->set_characteristics(std::move(fake_chrcs));

  fake_client()->set_characteristic_discovery_status(
      ToResult(HostError::kNotSupported));

  att::Result<> status1 = fit::ok();
  service->DiscoverCharacteristics(
      [&](att::Result<> cb_status, const auto& chrcs) {
        status1 = cb_status;
        EXPECT_TRUE(chrcs.empty());
      });

  // Queue a second request.
  att::Result<> status2 = fit::ok();
  service->DiscoverCharacteristics(
      [&](att::Result<> cb_status, const auto& chrcs) {
        status2 = cb_status;
        EXPECT_TRUE(chrcs.empty());
      });

  RunUntilIdle();
  // Only one request should have been made.
  EXPECT_EQ(1u, fake_client()->chrc_discovery_count());
  EXPECT_FALSE(service->IsDiscovered());
  EXPECT_EQ(ToResult(HostError::kNotSupported), status1);
  EXPECT_EQ(ToResult(HostError::kNotSupported), status2);
}

// Discover descriptors of a service with one characteristic.
TEST_F(RemoteServiceManagerTest, DiscoverDescriptorsOfOneSuccess) {
  ServiceData data(ServiceKind::PRIMARY, kStart, kEnd, kTestServiceUuid1);
  auto service = SetUpFakeService(data);

  CharacteristicData fake_chrc(
      0, std::nullopt, kCharDecl, kCharValue, kTestUuid3);
  fake_client()->set_characteristics({{fake_chrc}});

  DescriptorData fake_desc1(kDesc1, kTestUuid3);
  DescriptorData fake_desc2(kDesc2, kTestUuid4);
  fake_client()->set_descriptors({{fake_desc1, fake_desc2}});

  att::Result<> status = ToResult(HostError::kFailed);
  service->DiscoverCharacteristics(
      [&](att::Result<> cb_status, const auto chrcs) {
        status = cb_status;
        EXPECT_EQ(1u, chrcs.size());

        std::map<CharacteristicHandle,
                 std::pair<CharacteristicData,
                           std::map<DescriptorHandle, DescriptorData>>>
            expected = {
                {CharacteristicHandle(kCharValue),
                 {fake_chrc, {{kDesc1, fake_desc1}, {kDesc2, fake_desc2}}}}};

        EXPECT_EQ(expected, chrcs);
      });

  RunUntilIdle();
  EXPECT_EQ(1u, fake_client()->chrc_discovery_count());
  EXPECT_EQ(1u, fake_client()->desc_discovery_count());
  EXPECT_TRUE(service->IsDiscovered());
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(kDesc1, fake_client()->last_desc_discovery_start_handle());
  EXPECT_EQ(kEnd, fake_client()->last_desc_discovery_end_handle());
}

// Discover descriptors of a service with one characteristic.
TEST_F(RemoteServiceManagerTest, DiscoverDescriptorsOfOneError) {
  ServiceData data(ServiceKind::PRIMARY, kStart, kEnd, kTestServiceUuid1);
  auto service = SetUpFakeService(data);

  CharacteristicData fake_chrc(
      0, std::nullopt, kCharDecl, kCharValue, kTestUuid3);
  fake_client()->set_characteristics({{fake_chrc}});

  DescriptorData fake_desc1(kDesc1, kTestUuid3);
  DescriptorData fake_desc2(kDesc2, kTestUuid4);
  fake_client()->set_descriptors({{fake_desc1, fake_desc2}});
  fake_client()->set_descriptor_discovery_status(
      ToResult(HostError::kNotSupported));

  att::Result<> status = fit::ok();
  service->DiscoverCharacteristics(
      [&](att::Result<> cb_status, const auto& chrcs) {
        status = cb_status;
        EXPECT_TRUE(chrcs.empty());
      });

  RunUntilIdle();
  EXPECT_EQ(1u, fake_client()->chrc_discovery_count());
  EXPECT_EQ(1u, fake_client()->desc_discovery_count());
  EXPECT_FALSE(service->IsDiscovered());
  EXPECT_EQ(ToResult(HostError::kNotSupported), status);
}

// Discover descriptors of a service with multiple characteristics
TEST_F(RemoteServiceManagerTest, DiscoverDescriptorsOfMultipleSuccess) {
  // Has one descriptor
  CharacteristicData fake_char1(0, std::nullopt, 2, 3, kTestUuid3);
  DescriptorData fake_desc1(4, kTestUuid4);

  // Has no descriptors
  CharacteristicData fake_char2(0, std::nullopt, 5, 6, kTestUuid3);

  // Has two descriptors
  CharacteristicData fake_char3(0, std::nullopt, 7, 8, kTestUuid3);
  DescriptorData fake_desc2(9, kTestUuid4);
  DescriptorData fake_desc3(10, kTestUuid4);

  ServiceData data(
      ServiceKind::PRIMARY, 1, fake_desc3.handle, kTestServiceUuid1);
  auto service = SetUpFakeService(data);
  fake_client()->set_characteristics({{fake_char1, fake_char2, fake_char3}});
  fake_client()->set_descriptors({{fake_desc1, fake_desc2, fake_desc3}});

  att::Result<> status = ToResult(HostError::kFailed);
  service->DiscoverCharacteristics([&](att::Result<> cb_status,
                                       const auto& chrcs) {
    status = cb_status;

    std::map<CharacteristicHandle,
             std::pair<CharacteristicData,
                       std::map<DescriptorHandle, DescriptorData>>>
        expected = {{CharacteristicHandle(3), {fake_char1, {{4, fake_desc1}}}},
                    {CharacteristicHandle(6), {fake_char2, {}}},
                    {CharacteristicHandle(8),
                     {fake_char3, {{9, fake_desc2}, {10, fake_desc3}}}}};

    EXPECT_EQ(expected, chrcs);
  });

  RunUntilIdle();
  EXPECT_EQ(1u, fake_client()->chrc_discovery_count());
  // There should have been two descriptor discovery requests as discovery
  // should have been skipped for characteristic #2 due to its handles.
  EXPECT_EQ(2u, fake_client()->desc_discovery_count());
  EXPECT_TRUE(service->IsDiscovered());
  EXPECT_EQ(fit::ok(), status);
}

// Discover descriptors of a service with multiple characteristics. The first
// request results in an error though others succeed.
TEST_F(RemoteServiceManagerTest, DiscoverDescriptorsOfMultipleEarlyFail) {
  // Has one descriptor
  CharacteristicData fake_char1(0, std::nullopt, 2, 3, kTestUuid3);
  DescriptorData fake_desc1(4, kTestUuid4);

  // Has no descriptors
  CharacteristicData fake_char2(0, std::nullopt, 5, 6, kTestUuid3);

  // Has two descriptors
  CharacteristicData fake_char3(0, std::nullopt, 7, 8, kTestUuid3);
  DescriptorData fake_desc2(9, kTestUuid4);
  DescriptorData fake_desc3(10, kTestUuid4);

  ServiceData data(
      ServiceKind::PRIMARY, 1, fake_desc3.handle, kTestServiceUuid1);
  auto service = SetUpFakeService(data);
  fake_client()->set_characteristics({{fake_char1, fake_char2, fake_char3}});
  fake_client()->set_descriptors({{fake_desc1, fake_desc2, fake_desc3}});

  // The first request will fail
  fake_client()->set_descriptor_discovery_status(
      ToResult(HostError::kNotSupported), 1);

  att::Result<> status = ToResult(HostError::kFailed);
  service->DiscoverCharacteristics(
      [&](att::Result<> cb_status, const auto& chrcs) {
        status = cb_status;
        EXPECT_TRUE(chrcs.empty());
      });

  RunUntilIdle();
  EXPECT_EQ(1u, fake_client()->chrc_discovery_count());
  // There should have been two descriptor discovery requests as discovery
  // should have been skipped for characteristic #2 due to its handles.
  EXPECT_EQ(2u, fake_client()->desc_discovery_count());
  EXPECT_FALSE(service->IsDiscovered());
  EXPECT_EQ(ToResult(HostError::kNotSupported), status);
}

// Discover descriptors of a service with multiple characteristics. The last
// request results in an error while the preceding ones succeed.
TEST_F(RemoteServiceManagerTest, DiscoverDescriptorsOfMultipleLateFail) {
  // Has one descriptor
  CharacteristicData fake_char1(0, std::nullopt, 2, 3, kTestUuid3);
  DescriptorData fake_desc1(4, kTestUuid4);

  // Has no descriptors
  CharacteristicData fake_char2(0, std::nullopt, 5, 6, kTestUuid3);

  // Has two descriptors
  CharacteristicData fake_char3(0, std::nullopt, 7, 8, kTestUuid3);
  DescriptorData fake_desc2(9, kTestUuid4);
  DescriptorData fake_desc3(10, kTestUuid4);

  ServiceData data(
      ServiceKind::PRIMARY, 1, fake_desc3.handle, kTestServiceUuid1);
  auto service = SetUpFakeService(data);
  fake_client()->set_characteristics({{fake_char1, fake_char2, fake_char3}});
  fake_client()->set_descriptors({{fake_desc1, fake_desc2, fake_desc3}});

  // The last request will fail
  fake_client()->set_descriptor_discovery_status(
      ToResult(HostError::kNotSupported), 2);

  att::Result<> status = ToResult(HostError::kFailed);
  service->DiscoverCharacteristics(
      [&](att::Result<> cb_status, const auto& chrcs) {
        status = cb_status;
        EXPECT_TRUE(chrcs.empty());
      });

  RunUntilIdle();
  EXPECT_EQ(1u, fake_client()->chrc_discovery_count());
  // There should have been two descriptor discovery requests as discovery
  // should have been skipped for characteristic #2 due to its handles.
  EXPECT_EQ(2u, fake_client()->desc_discovery_count());
  EXPECT_FALSE(service->IsDiscovered());
  EXPECT_EQ(ToResult(HostError::kNotSupported), status);
}

// Discover descriptors of a service with extended properties set.
TEST_F(RemoteServiceManagerTest,
       DiscoverDescriptorsWithExtendedPropertiesSuccess) {
  ServiceData data(ServiceKind::PRIMARY, kStart, kEnd, kTestServiceUuid1);
  auto service = SetUpFakeService(data);

  // The ExtendedProperties of the characteristic is set.
  const Properties props = Property::kExtendedProperties;
  CharacteristicData fake_chrc(
      props, std::nullopt, kCharDecl, kCharValue, kTestUuid3);

  DescriptorData fake_desc1(kDesc1, types::kCharacteristicExtProperties);
  DescriptorData fake_desc2(kDesc2, kTestUuid4);

  SetCharacteristicsAndDescriptors({fake_chrc}, {fake_desc1, fake_desc2});

  // The callback should be triggered once to read the value of the descriptor
  // containing the ExtendedProperties bitfield.
  size_t read_cb_count = 0;
  auto extended_prop_read_cb = [&](att::Handle handle, auto callback) {
    EXPECT_EQ(kDesc1, handle);
    callback(fit::ok(), kExtendedPropValue, /*maybe_truncated=*/false);
    read_cb_count++;
  };
  fake_client()->set_read_request_callback(std::move(extended_prop_read_cb));

  att::Result<> status = ToResult(HostError::kFailed);
  service->DiscoverCharacteristics([&](att::Result<> cb_status,
                                       const auto chrcs) {
    status = cb_status;
    EXPECT_EQ(1u, chrcs.size());

    CharacteristicData expected_chrc(
        props, kReliableWrite, kCharDecl, kCharValue, kTestUuid3);
    std::map<CharacteristicHandle,
             std::pair<CharacteristicData,
                       std::map<DescriptorHandle, DescriptorData>>>
        expected = {
            {CharacteristicHandle(kCharValue),
             {expected_chrc, {{kDesc1, fake_desc1}, {kDesc2, fake_desc2}}}}};
    EXPECT_EQ(expected, chrcs);

    // Validate that the ExtendedProperties have been written to the |chrcs|
    // returned in the callback.
    CharacteristicData chrc_data =
        chrcs.at(CharacteristicHandle(kCharValue)).first;
    EXPECT_TRUE(chrc_data.extended_properties.has_value());
    EXPECT_EQ(ExtendedProperty::kReliableWrite,
              chrc_data.extended_properties.value());
  });

  RunUntilIdle();
  EXPECT_EQ(1u, fake_client()->chrc_discovery_count());
  EXPECT_EQ(1u, fake_client()->desc_discovery_count());
  EXPECT_EQ(1u, read_cb_count);
  EXPECT_TRUE(service->IsDiscovered());
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(kDesc1, fake_client()->last_desc_discovery_start_handle());
  EXPECT_EQ(kEnd, fake_client()->last_desc_discovery_end_handle());
}

// Discover descriptors of a service that doesn't contain the ExtendedProperties
// bit set, but with a descriptor containing an ExtendedProperty value. This is
// not invalid, as per the spec, and so discovery shouldn't fail.
TEST_F(RemoteServiceManagerTest, DiscoverDescriptorsExtendedPropertiesNotSet) {
  ServiceData data(ServiceKind::PRIMARY, kStart, kEnd, kTestServiceUuid1);
  auto service = SetUpFakeService(data);

  // The ExtendedProperties of the characteristic is not set.
  CharacteristicData fake_chrc(
      0, std::nullopt, kCharDecl, kCharValue, kTestUuid3);
  DescriptorData fake_desc1(kDesc1, types::kCharacteristicExtProperties);
  SetCharacteristicsAndDescriptors({fake_chrc}, {fake_desc1});

  // Callback should not be executed.
  size_t read_cb_count = 0;
  auto extended_prop_read_cb = [&](att::Handle, auto callback) {
    callback(fit::ok(), kExtendedPropValue, /*maybe_truncated=*/false);
    read_cb_count++;
  };
  fake_client()->set_read_request_callback(std::move(extended_prop_read_cb));

  att::Result<> status = ToResult(HostError::kFailed);
  service->DiscoverCharacteristics(
      [&](att::Result<> cb_status, const auto chrcs) {
        status = cb_status;
        EXPECT_EQ(1u, chrcs.size());

        std::map<CharacteristicHandle,
                 std::pair<CharacteristicData,
                           std::map<DescriptorHandle, DescriptorData>>>
            expected = {{CharacteristicHandle(kCharValue),
                         {fake_chrc, {{kDesc1, fake_desc1}}}}};

        EXPECT_EQ(expected, chrcs);

        // Validate that the ExtendedProperties has not been updated.
        CharacteristicData chrc_data =
            chrcs.at(CharacteristicHandle(kCharValue)).first;
        EXPECT_FALSE(chrc_data.extended_properties.has_value());
      });

  RunUntilIdle();
  EXPECT_EQ(1u, fake_client()->chrc_discovery_count());
  EXPECT_EQ(1u, fake_client()->desc_discovery_count());
  EXPECT_EQ(0u, read_cb_count);
  EXPECT_TRUE(service->IsDiscovered());
  EXPECT_EQ(fit::ok(), status);
}

// Discover descriptors of a service with two descriptors containing
// ExtendedProperties. This is invalid, and discovery should fail.
TEST_F(RemoteServiceManagerTest,
       DiscoverDescriptorsMultipleExtendedPropertiesError) {
  ServiceData data(ServiceKind::PRIMARY, kStart, kEnd, kTestServiceUuid1);
  auto service = SetUpFakeService(data);

  // The ExtendedProperties of the characteristic is set.
  const Properties props = Property::kExtendedProperties;
  CharacteristicData fake_chrc(
      props, std::nullopt, kCharDecl, kCharValue, kTestUuid3);
  // Two descriptors with ExtProperties.
  DescriptorData fake_desc1(kDesc1, types::kCharacteristicExtProperties);
  DescriptorData fake_desc2(kDesc2, types::kCharacteristicExtProperties);
  SetCharacteristicsAndDescriptors({fake_chrc}, {fake_desc1, fake_desc2});

  size_t read_cb_count = 0;
  auto extended_prop_read_cb = [&](att::Handle, auto callback) {
    callback(fit::ok(), kExtendedPropValue, /*maybe_truncated=*/false);
    read_cb_count++;
  };
  fake_client()->set_read_request_callback(std::move(extended_prop_read_cb));

  att::Result<> status = ToResult(HostError::kFailed);
  service->DiscoverCharacteristics(
      [&](att::Result<> cb_status, const auto chrcs) {
        status = cb_status;
        EXPECT_TRUE(chrcs.empty());
      });

  RunUntilIdle();
  EXPECT_EQ(1u, fake_client()->chrc_discovery_count());
  EXPECT_EQ(1u, fake_client()->desc_discovery_count());
  EXPECT_EQ(0u, read_cb_count);
  EXPECT_FALSE(service->IsDiscovered());
  EXPECT_EQ(ToResult(HostError::kFailed), status);
}

// Discover descriptors of a service with ExtendedProperties set, but with
// an error when reading the descriptor value. Discovery should fail.
TEST_F(RemoteServiceManagerTest,
       DiscoverDescriptorsExtendedPropertiesReadDescValueError) {
  ServiceData data(ServiceKind::PRIMARY, kStart, kEnd, kTestServiceUuid1);
  auto service = SetUpFakeService(data);

  // The ExtendedProperties of the characteristic is set.
  const Properties props = Property::kExtendedProperties;
  CharacteristicData fake_chrc(
      props, std::nullopt, kCharDecl, kCharValue, kTestUuid3);
  DescriptorData fake_desc1(kDesc1, types::kCharacteristicExtProperties);
  DescriptorData fake_desc2(kDesc2, kTestUuid4);
  SetCharacteristicsAndDescriptors({fake_chrc}, {fake_desc1, fake_desc2});

  // The callback should be triggered once to read the value of the descriptor
  // containing the ExtendedProperties bitfield.
  size_t read_cb_count = 0;
  auto extended_prop_read_cb = [&](att::Handle handle, auto callback) {
    EXPECT_EQ(kDesc1, handle);
    callback(ToResult(att::ErrorCode::kReadNotPermitted),
             BufferView(),
             /*maybe_truncated=*/false);
    read_cb_count++;
  };
  fake_client()->set_read_request_callback(std::move(extended_prop_read_cb));

  att::Result<> status = ToResult(HostError::kFailed);
  service->DiscoverCharacteristics(
      [&](att::Result<> cb_status, const auto chrcs) {
        status = cb_status;
        EXPECT_TRUE(chrcs.empty());
      });

  RunUntilIdle();

  EXPECT_EQ(1u, read_cb_count);
  EXPECT_FALSE(service->IsDiscovered());
  ASSERT_TRUE(status.is_error());
  EXPECT_TRUE(status.error_value().is_protocol_error());
}

// Discover descriptors of a service with ExtendedProperties set, but with
// a malformed response when reading the descriptor value. Discovery should
// fail.
TEST_F(RemoteServiceManagerTest,
       DiscoverDescriptorsExtendedPropertiesReadDescInvalidValue) {
  ServiceData data(ServiceKind::PRIMARY, kStart, kEnd, kTestServiceUuid1);
  auto service = SetUpFakeService(data);

  // The ExtendedProperties of the characteristic is set.
  const Properties props = Property::kExtendedProperties;
  CharacteristicData fake_chrc(
      props, std::nullopt, kCharDecl, kCharValue, kTestUuid3);
  DescriptorData fake_desc1(kDesc1, types::kCharacteristicExtProperties);
  DescriptorData fake_desc2(kDesc2, kTestUuid4);
  SetCharacteristicsAndDescriptors({fake_chrc}, {fake_desc1, fake_desc2});

  // The callback should be triggered once to read the value of the descriptor
  // containing the ExtendedProperties bitfield.
  size_t read_cb_count = 0;
  auto extended_prop_read_cb = [&](att::Handle handle, auto callback) {
    EXPECT_EQ(kDesc1, handle);
    callback(fit::ok(),
             BufferView(),
             /*maybe_truncated=*/false);  // Invalid return buf
    read_cb_count++;
  };
  fake_client()->set_read_request_callback(std::move(extended_prop_read_cb));

  att::Result<> status = ToResult(HostError::kFailed);
  service->DiscoverCharacteristics(
      [&](att::Result<> cb_status, const auto chrcs) {
        status = cb_status;
        EXPECT_TRUE(chrcs.empty());
      });

  RunUntilIdle();
  EXPECT_EQ(1u, fake_client()->chrc_discovery_count());
  EXPECT_EQ(1u, fake_client()->desc_discovery_count());
  EXPECT_EQ(1u, read_cb_count);
  EXPECT_FALSE(service->IsDiscovered());
  EXPECT_EQ(ToResult(HostError::kPacketMalformed), status);
}

constexpr CharacteristicHandle kDefaultCharacteristic(3);
constexpr CharacteristicHandle kSecondCharacteristic(6);
constexpr CharacteristicHandle kInvalidCharacteristic(1);

constexpr att::Handle kDefaultChrcValueHandle = 3;

CharacteristicData UnreadableChrc() {
  return CharacteristicData(
      0, std::nullopt, 2, kDefaultChrcValueHandle, kTestUuid3);
}
CharacteristicData ReadableChrc() {
  return CharacteristicData(
      Property::kRead, std::nullopt, 2, kDefaultChrcValueHandle, kTestUuid3);
}
CharacteristicData WritableChrc() {
  return CharacteristicData(
      Property::kWrite, std::nullopt, 2, kDefaultChrcValueHandle, kTestUuid3);
}
CharacteristicData WriteableExtendedPropChrc() {
  uint8_t props = Property::kWrite | Property::kExtendedProperties;
  return CharacteristicData(
      props, std::nullopt, 2, kDefaultChrcValueHandle, kTestUuid3);
}

TEST_F(RemoteServiceManagerTest, ReadCharWhileNotReady) {
  auto service = SetUpFakeService(
      ServiceData(ServiceKind::PRIMARY, 1, 2, kTestServiceUuid1));

  att::Result<> status = fit::ok();
  service->ReadCharacteristic(
      kDefaultCharacteristic,
      [&](att::Result<> cb_status, const auto&, auto) { status = cb_status; });

  RunUntilIdle();
  EXPECT_EQ(ToResult(HostError::kNotReady), status);
}

TEST_F(RemoteServiceManagerTest, ReadCharNotFound) {
  auto service = SetupServiceWithChrcs(
      ServiceData(ServiceKind::PRIMARY, 1, 2, kTestServiceUuid1), {});
  att::Result<> status = fit::ok();
  service->ReadCharacteristic(
      kDefaultCharacteristic,
      [&](att::Result<> cb_status, const auto&, auto) { status = cb_status; });

  RunUntilIdle();
  EXPECT_EQ(ToResult(HostError::kNotFound), status);
}

TEST_F(RemoteServiceManagerTest, ReadCharNotSupported) {
  auto service = SetupServiceWithChrcs(
      ServiceData(ServiceKind::PRIMARY, 1, 3, kTestServiceUuid1),
      {UnreadableChrc()});
  att::Result<> status = fit::ok();
  service->ReadCharacteristic(
      kDefaultCharacteristic,
      [&](att::Result<> cb_status, const auto&, auto) { status = cb_status; });

  RunUntilIdle();
  EXPECT_EQ(ToResult(HostError::kNotSupported), status);
}

TEST_F(RemoteServiceManagerTest, ReadCharSendsReadRequest) {
  auto service = SetupServiceWithChrcs(
      ServiceData(
          ServiceKind::PRIMARY, 1, kDefaultChrcValueHandle, kTestServiceUuid1),
      {ReadableChrc()});

  const StaticByteBuffer kValue('t', 'e', 's', 't');

  fake_client()->set_read_request_callback(
      [&](att::Handle handle, auto callback) {
        EXPECT_EQ(kDefaultChrcValueHandle, handle);
        callback(fit::ok(), kValue, /*maybe_truncated=*/false);
      });

  att::Result<> status = ToResult(HostError::kFailed);
  service->ReadCharacteristic(
      kDefaultCharacteristic,
      [&](att::Result<> cb_status, const auto& value, bool maybe_truncated) {
        status = cb_status;
        EXPECT_TRUE(ContainersEqual(kValue, value));
        EXPECT_FALSE(maybe_truncated);
      });

  RunUntilIdle();

  EXPECT_EQ(fit::ok(), status);
}

TEST_F(RemoteServiceManagerTest, ReadLongWhileNotReady) {
  auto service = SetUpFakeService(
      ServiceData(ServiceKind::PRIMARY, 1, 2, kTestServiceUuid1));

  att::Result<> status = fit::ok();
  service->ReadLongCharacteristic(
      CharacteristicHandle(0),
      0,
      512,
      [&](att::Result<> cb_status, const auto&, auto) { status = cb_status; });

  RunUntilIdle();

  EXPECT_EQ(ToResult(HostError::kNotReady), status);
}

TEST_F(RemoteServiceManagerTest, ReadLongNotFound) {
  auto service = SetupServiceWithChrcs(
      ServiceData(ServiceKind::PRIMARY, 1, 2, kTestServiceUuid1), {});

  att::Result<> status = fit::ok();
  service->ReadLongCharacteristic(
      CharacteristicHandle(0),
      0,
      512,
      [&](att::Result<> cb_status, const auto&, auto) { status = cb_status; });

  RunUntilIdle();

  EXPECT_EQ(ToResult(HostError::kNotFound), status);
}

TEST_F(RemoteServiceManagerTest, ReadLongNotSupported) {
  auto service = SetupServiceWithChrcs(
      ServiceData(ServiceKind::PRIMARY, 1, 3, kTestServiceUuid1),
      {UnreadableChrc()});

  att::Result<> status = fit::ok();
  service->ReadLongCharacteristic(
      kDefaultCharacteristic,
      0,
      512,
      [&](att::Result<> cb_status, const auto&, auto) { status = cb_status; });

  RunUntilIdle();

  EXPECT_EQ(ToResult(HostError::kNotSupported), status);
}

// 0 is not a valid parameter for the |max_size| field of ReadLongCharacteristic
TEST_F(RemoteServiceManagerTest, ReadLongMaxSizeZero) {
  auto service = SetupServiceWithChrcs(
      ServiceData(ServiceKind::PRIMARY, 1, 3, kTestServiceUuid1),
      {ReadableChrc()});

  att::Result<> status = fit::ok();
  service->ReadLongCharacteristic(
      kDefaultCharacteristic,
      0,
      0,
      [&](att::Result<> cb_status, const auto&, auto) { status = cb_status; });

  RunUntilIdle();

  EXPECT_EQ(ToResult(HostError::kInvalidParameters), status);
}

// The complete attribute value is read in a single request.
TEST_F(RemoteServiceManagerTest, ReadLongSingleBlob) {
  constexpr uint16_t kOffset = 0;
  constexpr size_t kMaxBytes = 1000;

  auto service = SetupServiceWithChrcs(
      ServiceData(
          ServiceKind::PRIMARY, 1, kDefaultChrcValueHandle, kTestServiceUuid1),
      {ReadableChrc()});

  const StaticByteBuffer kValue('t', 'e', 's', 't');

  int request_count = 0;
  fake_client()->set_read_request_callback(
      [&](att::Handle handle, auto callback) {
        request_count++;
        EXPECT_EQ(request_count, 1);
        EXPECT_EQ(kDefaultChrcValueHandle, handle);
        callback(fit::ok(), kValue, /*maybe_truncated=*/false);
      });

  att::Result<> status = ToResult(HostError::kFailed);
  service->ReadLongCharacteristic(
      kDefaultCharacteristic,
      kOffset,
      kMaxBytes,
      [&](att::Result<> cb_status, const auto& value, bool maybe_truncated) {
        status = cb_status;
        EXPECT_TRUE(ContainersEqual(kValue, value));
        EXPECT_FALSE(maybe_truncated);
      });

  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
}

TEST_F(RemoteServiceManagerTest, ReadLongMultipleBlobs) {
  constexpr uint16_t kOffset = 0;
  constexpr size_t kMaxBytes = 1000;
  constexpr int kExpectedBlobCount = 4;

  auto service = SetupServiceWithChrcs(
      ServiceData(
          ServiceKind::PRIMARY, 1, kDefaultChrcValueHandle, kTestServiceUuid1),
      {ReadableChrc()});

  // Create a buffer that will take 4 requests to read. Since the default MTU is
  // 23:
  //   a. The size of |expected_value| is 69.
  //   b. We should read 22 + 22 + 22 + 3 bytes across 4 requests.
  StaticByteBuffer<att::kLEMinMTU * 3> expected_value;

  // Initialize the contents.
  for (uint8_t i = 0; i < expected_value.size(); ++i) {
    expected_value[i] = i;
  }

  int read_count = 0;
  fake_client()->set_read_request_callback(
      [&](att::Handle handle, auto callback) {
        read_count++;
        EXPECT_EQ(read_count, 1);
        EXPECT_EQ(kDefaultChrcValueHandle, handle);
        auto blob = expected_value.view(0, att::kLEMinMTU - 1);
        callback(fit::ok(), blob, /*maybe_truncated=*/true);
      });

  fake_client()->set_read_blob_request_callback(
      [&](att::Handle handle, uint16_t offset, auto callback) {
        read_count++;
        EXPECT_GT(read_count, 1);
        EXPECT_EQ(kDefaultChrcValueHandle, handle);
        bool maybe_truncated = true;

        // Return a blob at the given offset with at most MTU - 1 bytes.
        auto blob = expected_value.view(offset, att::kLEMinMTU - 1);
        if (read_count == kExpectedBlobCount) {
          // The final blob should contain 3 bytes.
          EXPECT_EQ(3u, blob.size());
          maybe_truncated = false;
        }

        callback(fit::ok(), blob, maybe_truncated);
      });

  att::Result<> status = ToResult(HostError::kFailed);
  service->ReadLongCharacteristic(
      kDefaultCharacteristic,
      kOffset,
      kMaxBytes,
      [&](att::Result<> cb_status, const auto& value, bool maybe_truncated) {
        status = cb_status;
        EXPECT_TRUE(ContainersEqual(expected_value, value));
        EXPECT_FALSE(maybe_truncated);
      });

  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(kExpectedBlobCount, read_count);
}

// Simulates a peer that rejects a read blob request with a kAttributeNotLong
// error. The initial read request completes successfully and contains the
// entire value. The specification implies that the peer can either respond with
// an empty buffer or a kAttributeNotLong error for the second request.
TEST_F(RemoteServiceManagerTest,
       ReadLongCharacteristicAttributeNotLongErrorIgnored) {
  constexpr uint16_t kOffset = 0;
  constexpr size_t kMaxBytes = 1000;
  constexpr int kExpectedBlobCount = 2;

  auto service = SetupServiceWithChrcs(
      ServiceData(
          ServiceKind::PRIMARY, 1, kDefaultChrcValueHandle, kTestServiceUuid1),
      {ReadableChrc()});

  StaticByteBuffer<att::kLEMinMTU - 1> expected_value;
  for (uint8_t i = 0; i < expected_value.size(); ++i) {
    expected_value[i] = i;
  }

  int read_count = 0;
  fake_client()->set_read_request_callback([&](att::Handle, auto callback) {
    read_count++;
    EXPECT_EQ(read_count, 1);
    callback(fit::ok(), expected_value.view(), /*maybe_truncated=*/true);
  });

  fake_client()->set_read_blob_request_callback(
      [&](att::Handle, uint16_t, auto callback) {
        read_count++;
        EXPECT_EQ(read_count, 2);
        callback(ToResult(att::ErrorCode::kAttributeNotLong),
                 BufferView(),
                 /*maybe_truncated=*/false);
      });

  att::Result<> status = ToResult(HostError::kFailed);
  service->ReadLongCharacteristic(
      kDefaultCharacteristic,
      kOffset,
      kMaxBytes,
      [&](att::Result<> cb_status, const auto& value, bool maybe_truncated) {
        status = cb_status;
        EXPECT_TRUE(ContainersEqual(expected_value, value));
        EXPECT_FALSE(maybe_truncated);
      });

  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(kExpectedBlobCount, read_count);
}

TEST_F(RemoteServiceManagerTest,
       ReadLongCharacteristicAttributeNotLongErrorOnFirstReadRequest) {
  constexpr uint16_t kOffset = 0;
  constexpr size_t kMaxBytes = 1000;
  constexpr int kExpectedBlobCount = 1;

  auto service = SetupServiceWithChrcs(
      ServiceData(
          ServiceKind::PRIMARY, 1, kDefaultChrcValueHandle, kTestServiceUuid1),
      {ReadableChrc()});

  int read_count = 0;
  fake_client()->set_read_request_callback([&](att::Handle, auto callback) {
    read_count++;
    EXPECT_EQ(read_count, 1);
    callback(ToResult(att::ErrorCode::kAttributeNotLong),
             BufferView(),
             /*maybe_truncated=*/false);
  });
  fake_client()->set_read_blob_request_callback(
      [&](auto, auto, auto) { FAIL(); });

  att::Result<> status = fit::ok();
  service->ReadLongCharacteristic(
      kDefaultCharacteristic,
      kOffset,
      kMaxBytes,
      [&](att::Result<> cb_status, const auto&, bool maybe_truncated) {
        status = cb_status;
        EXPECT_FALSE(maybe_truncated);
      });

  RunUntilIdle();
  EXPECT_EQ(ToResult(att::ErrorCode::kAttributeNotLong), status);
  EXPECT_EQ(kExpectedBlobCount, read_count);
}

// Same as ReadLongMultipleBlobs except the characteristic value has a size that
// is a multiple of (ATT_MTU - 1), so that the last read blob request returns 0
// bytes.
TEST_F(RemoteServiceManagerTest, ReadLongValueExactMultipleOfMTU) {
  constexpr uint16_t kOffset = 0;
  constexpr size_t kMaxBytes = 1000;
  constexpr int kExpectedBlobCount = 4;

  auto service = SetupServiceWithChrcs(
      ServiceData(
          ServiceKind::PRIMARY, 1, kDefaultChrcValueHandle, kTestServiceUuid1),
      {ReadableChrc()});

  // Create a buffer that will take 4 requests to read. Since the default MTU is
  // 23:
  //   a. The size of |expected_value| is 66.
  //   b. We should read 22 + 22 + 22 + 0 bytes across 4 requests.
  StaticByteBuffer<(att::kLEMinMTU - 1) * 3> expected_value;

  // Initialize the contents.
  for (uint8_t i = 0; i < expected_value.size(); ++i) {
    expected_value[i] = i;
  }

  int read_count = 0;
  fake_client()->set_read_request_callback(
      [&](att::Handle handle, auto callback) {
        read_count++;
        EXPECT_EQ(read_count, 1);
        EXPECT_EQ(kDefaultChrcValueHandle, handle);
        auto blob = expected_value.view(0, att::kLEMinMTU - 1);
        callback(fit::ok(), blob, /*maybe_truncated=*/true);
      });

  fake_client()->set_read_blob_request_callback(
      [&](att::Handle handle, uint16_t offset, auto callback) {
        read_count++;
        EXPECT_GT(read_count, 1);
        EXPECT_EQ(kDefaultChrcValueHandle, handle);
        bool maybe_truncated = true;

        // Return a blob at the given offset with at most MTU - 1 bytes.
        auto blob = expected_value.view(offset, att::kLEMinMTU - 1);
        if (read_count == kExpectedBlobCount) {
          // The final blob should be empty.
          EXPECT_EQ(0u, blob.size());
          maybe_truncated = false;
        }

        callback(fit::ok(), blob, maybe_truncated);
      });

  att::Result<> status = ToResult(HostError::kFailed);
  service->ReadLongCharacteristic(
      kDefaultCharacteristic,
      kOffset,
      kMaxBytes,
      [&](att::Result<> cb_status, const auto& value, bool maybe_truncated) {
        status = cb_status;
        EXPECT_TRUE(ContainersEqual(expected_value, value));
        EXPECT_FALSE(maybe_truncated);
      });

  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(kExpectedBlobCount, read_count);
}

// Same as ReadLongMultipleBlobs but a maximum size is given that is smaller
// than the size of the attribute value.
TEST_F(RemoteServiceManagerTest, ReadLongMultipleBlobsWithMaxSize) {
  constexpr uint16_t kOffset = 0;
  constexpr size_t kMaxBytes = 40;
  constexpr int kExpectedBlobCount = 2;

  auto service = SetupServiceWithChrcs(
      ServiceData(
          ServiceKind::PRIMARY, 1, kDefaultChrcValueHandle, kTestServiceUuid1),
      {ReadableChrc()});

  // Reads will return 22 + 22 bytes across 2 requests, but only 18 bytes of the
  // second read will be reported to ReadLongCharacteristic (the value will be
  // truncated).
  StaticByteBuffer<att::kLEMinMTU * 3> expected_value;

  // Initialize the contents.
  for (uint8_t i = 0; i < expected_value.size(); ++i) {
    expected_value[i] = i;
  }

  int read_count = 0;
  fake_client()->set_read_request_callback(
      [&](att::Handle handle, auto callback) {
        read_count++;
        EXPECT_EQ(read_count, 1);
        EXPECT_EQ(kDefaultChrcValueHandle, handle);
        BufferView blob = expected_value.view(0, att::kLEMinMTU - 1);
        callback(fit::ok(), blob, /*maybe_truncated=*/true);
      });

  fake_client()->set_read_blob_request_callback(
      [&](att::Handle handle, uint16_t offset, auto callback) {
        read_count++;
        EXPECT_GT(read_count, 1);
        EXPECT_EQ(kDefaultChrcValueHandle, handle);
        BufferView blob = expected_value.view(offset, att::kLEMinMTU - 1);
        callback(fit::ok(), blob, /*maybe_truncated=*/true);
      });

  att::Result<> status = ToResult(HostError::kFailed);
  service->ReadLongCharacteristic(
      kDefaultCharacteristic,
      kOffset,
      kMaxBytes,
      [&](att::Result<> cb_status, const auto& value, bool maybe_truncated) {
        status = cb_status;
        EXPECT_TRUE(ContainersEqual(expected_value.view(0, kMaxBytes), value));
        EXPECT_TRUE(maybe_truncated);
      });

  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(kExpectedBlobCount, read_count);
}

// Same as ReadLongMultipleBlobs but a non-zero offset is given.
TEST_F(RemoteServiceManagerTest, ReadLongAtOffset) {
  constexpr uint16_t kOffset = 30;
  constexpr size_t kMaxBytes = 1000;
  constexpr int kExpectedBlobCount = 2;

  auto service = SetupServiceWithChrcs(
      ServiceData(
          ServiceKind::PRIMARY, 1, kDefaultChrcValueHandle, kTestServiceUuid1),
      {ReadableChrc()});

  // Size: 69.
  // Reads starting at offset 30 will return 22 + 17 bytes across 2 requests.
  StaticByteBuffer<att::kLEMinMTU * 3> expected_value;

  // Initialize the contents.
  for (uint8_t i = 0; i < expected_value.size(); ++i) {
    expected_value[i] = i;
  }

  int read_blob_count = 0;
  fake_client()->set_read_blob_request_callback(
      [&](att::Handle handle, uint16_t offset, auto callback) {
        EXPECT_EQ(kDefaultChrcValueHandle, handle);
        read_blob_count++;
        BufferView blob = expected_value.view(offset, att::kLEMinMTU - 1);
        bool maybe_truncated = (read_blob_count != kExpectedBlobCount);
        callback(fit::ok(), blob, maybe_truncated);
      });

  att::Result<> status = ToResult(HostError::kFailed);
  service->ReadLongCharacteristic(
      kDefaultCharacteristic,
      kOffset,
      kMaxBytes,
      [&](att::Result<> cb_status, const auto& value, bool maybe_truncated) {
        status = cb_status;
        EXPECT_TRUE(
            ContainersEqual(expected_value.view(kOffset, kMaxBytes), value));
        EXPECT_FALSE(maybe_truncated);
      });

  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(kExpectedBlobCount, read_blob_count);
}

// Same as ReadLongAtOffset but a very small max size is given.
TEST_F(RemoteServiceManagerTest, ReadLongAtOffsetWithMaxBytes) {
  constexpr uint16_t kOffset = 10;
  constexpr size_t kMaxBytes = 34;
  constexpr int kExpectedBlobCount = 2;

  auto service = SetupServiceWithChrcs(
      ServiceData(
          ServiceKind::PRIMARY, 1, kDefaultChrcValueHandle, kTestServiceUuid1),
      {ReadableChrc()});

  // Size: 69.
  // Reads starting at offset 10 will return 22 + 22 bytes across 2 requests,
  // but the second read value will be truncated to 12 bytes by RemoteService
  // due to |kMaxBytes|. A third read blob should not be sent since this should
  // satisfy |kMaxBytes|.
  StaticByteBuffer<att::kLEMinMTU * 3> expected_value;

  // Initialize the contents.
  for (size_t i = 0; i < expected_value.size(); ++i) {
    expected_value[i] = static_cast<uint8_t>(i);
  }

  int read_blob_count = 0;
  fake_client()->set_read_blob_request_callback(
      [&](att::Handle handle, uint16_t offset, auto callback) {
        EXPECT_EQ(kDefaultChrcValueHandle, handle);
        read_blob_count++;
        BufferView blob = expected_value.view(offset, att::kLEMinMTU - 1);
        callback(fit::ok(), blob, /*maybe_truncated=*/true);
      });

  att::Result<> status = ToResult(HostError::kFailed);
  service->ReadLongCharacteristic(
      kDefaultCharacteristic,
      kOffset,
      kMaxBytes,
      [&](att::Result<> cb_status, const auto& value, bool maybe_truncated) {
        status = cb_status;
        EXPECT_TRUE(
            ContainersEqual(expected_value.view(kOffset, kMaxBytes), value));
        EXPECT_TRUE(maybe_truncated);
      });

  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(kExpectedBlobCount, read_blob_count);
}

TEST_F(RemoteServiceManagerTest, ReadLongError) {
  constexpr uint16_t kOffset = 0;
  constexpr size_t kMaxBytes = 1000;
  constexpr int kExpectedBlobCount = 2;  // The second request will fail.

  auto service = SetupServiceWithChrcs(
      ServiceData(
          ServiceKind::PRIMARY, 1, kDefaultChrcValueHandle, kTestServiceUuid1),
      {ReadableChrc()});

  // Make the first blob large enough that it will cause a second read blob
  // request.
  StaticByteBuffer<att::kLEMinMTU - 1> first_blob;

  int read_count = 0;
  fake_client()->set_read_request_callback(
      [&](att::Handle handle, auto callback) {
        read_count++;
        EXPECT_EQ(read_count, 1);
        EXPECT_EQ(kDefaultChrcValueHandle, handle);
        callback(fit::ok(), first_blob, /*maybe_truncated=*/true);
      });

  fake_client()->set_read_blob_request_callback(
      [&](att::Handle handle, uint16_t, auto callback) {
        read_count++;
        EXPECT_EQ(read_count, 2);
        EXPECT_EQ(kDefaultChrcValueHandle, handle);
        callback(ToResult(att::ErrorCode::kInvalidOffset),
                 BufferView(),
                 /*maybe_truncated=*/false);
      });

  att::Result<> status = fit::ok();
  service->ReadLongCharacteristic(
      kDefaultCharacteristic,
      kOffset,
      kMaxBytes,
      [&](att::Result<> cb_status, const auto& value, bool maybe_truncated) {
        status = cb_status;
        EXPECT_EQ(0u, value.size());  // No value should be returned on error.
        EXPECT_FALSE(maybe_truncated);
      });

  RunUntilIdle();
  EXPECT_EQ(ToResult(att::ErrorCode::kInvalidOffset), status);
  EXPECT_EQ(kExpectedBlobCount, read_count);
}

TEST_F(RemoteServiceManagerTest,
       ReadByTypeSendsReadRequestsUntilAttributeNotFound) {
  constexpr att::Handle kStartHandle = 1;
  constexpr att::Handle kEndHandle = 5;
  auto service = SetUpFakeService(ServiceData(
      ServiceKind::PRIMARY, kStartHandle, kEndHandle, kTestServiceUuid1));

  constexpr UUID kCharUuid(uint16_t{0xfefe});

  constexpr att::Handle kHandle0 = 2;
  const StaticByteBuffer kValue0(0x00, 0x01, 0x02);
  const std::vector<Client::ReadByTypeValue> kValues0 = {
      {kHandle0, kValue0.view(), /*maybe_truncated=*/false}};

  constexpr att::Handle kHandle1 = 3;
  const StaticByteBuffer kValue1(0x03, 0x04, 0x05);
  const std::vector<Client::ReadByTypeValue> kValues1 = {
      {kHandle1, kValue1.view(), /*maybe_truncated=*/true}};

  size_t read_count = 0;
  fake_client()->set_read_by_type_request_callback(
      [&](const UUID&, att::Handle start, att::Handle, auto callback) {
        switch (read_count++) {
          case 0:
            EXPECT_EQ(kStartHandle, start);
            callback(fit::ok(kValues0));
            break;
          case 1:
            EXPECT_EQ(kHandle0 + 1, start);
            callback(fit::ok(kValues1));
            break;
          case 2:
            EXPECT_EQ(kHandle1 + 1, start);
            callback(fit::error(Client::ReadByTypeError{
                att::Error(att::ErrorCode::kAttributeNotFound), start}));
            break;
          default:
            FAIL();
        }
      });

  std::optional<att::Result<>> status;
  service->ReadByType(
      kCharUuid,
      [&](att::Result<> cb_status,
          std::vector<RemoteService::ReadByTypeResult> values) {
        status = cb_status;
        EXPECT_EQ(fit::ok(), *status);
        ASSERT_EQ(2u, values.size());
        EXPECT_EQ(CharacteristicHandle(kHandle0), values[0].handle);
        ASSERT_EQ(fit::ok(), values[0].result);
        EXPECT_TRUE(ContainersEqual(kValue0, *values[0].result.value()));
        EXPECT_FALSE(values[0].maybe_truncated);
        EXPECT_EQ(CharacteristicHandle(kHandle1), values[1].handle);
        ASSERT_EQ(fit::ok(), values[1].result);
        EXPECT_TRUE(ContainersEqual(kValue1, *values[1].result.value()));
        EXPECT_TRUE(values[1].maybe_truncated);
      });

  RunUntilIdle();
  ASSERT_TRUE(status.has_value());
  // kAttributeNotFound error should be treated as success.
  EXPECT_EQ(fit::ok(), *status);
}

TEST_F(RemoteServiceManagerTest,
       ReadByTypeSendsReadRequestsUntilServiceEndHandle) {
  constexpr att::Handle kStartHandle = 1;
  constexpr att::Handle kEndHandle = 2;
  auto service = SetUpFakeService(ServiceData(
      ServiceKind::PRIMARY, kStartHandle, kEndHandle, kTestServiceUuid1));

  constexpr UUID kCharUuid(uint16_t{0xfefe});

  constexpr att::Handle kHandle = kEndHandle;
  const StaticByteBuffer kValue(0x00, 0x01, 0x02);
  const std::vector<Client::ReadByTypeValue> kValues = {
      {kHandle, kValue.view(), /*maybe_truncated=*/false}};

  size_t read_count = 0;
  fake_client()->set_read_by_type_request_callback(
      [&](const UUID&, att::Handle start, att::Handle, auto callback) {
        EXPECT_EQ(kStartHandle, start);
        EXPECT_EQ(0u, read_count++);
        callback(fit::ok(kValues));
      });

  std::optional<att::Result<>> status;
  service->ReadByType(kCharUuid, [&](att::Result<> cb_status, auto values) {
    status = cb_status;
    ASSERT_EQ(1u, values.size());
    EXPECT_EQ(CharacteristicHandle(kHandle), values[0].handle);
    ASSERT_EQ(fit::ok(), values[0].result);
    EXPECT_TRUE(ContainersEqual(kValue, *values[0].result.value()));
  });

  RunUntilIdle();
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(fit::ok(), *status);
}

TEST_F(RemoteServiceManagerTest, ReadByTypeReturnsReadErrorsWithResults) {
  constexpr att::Handle kStartHandle = 1;
  constexpr att::Handle kEndHandle = 5;
  auto service = SetUpFakeService(ServiceData(
      ServiceKind::PRIMARY, kStartHandle, kEndHandle, kTestServiceUuid1));

  constexpr UUID kCharUuid(uint16_t{0xfefe});

  const std::array<att::ErrorCode, 5> errors = {
      att::ErrorCode::kInsufficientAuthorization,
      att::ErrorCode::kInsufficientAuthentication,
      att::ErrorCode::kInsufficientEncryptionKeySize,
      att::ErrorCode::kInsufficientEncryption,
      att::ErrorCode::kReadNotPermitted};

  size_t read_count = 0;
  fake_client()->set_read_by_type_request_callback(
      [&](const UUID&, att::Handle start, att::Handle, auto callback) {
        if (read_count < errors.size()) {
          EXPECT_EQ(kStartHandle + read_count, start);
          callback(fit::error(Client::ReadByTypeError{
              att::Error(errors[read_count++]), start}));
        } else {
          FAIL();
        }
      });

  std::optional<att::Result<>> status;
  service->ReadByType(kCharUuid, [&](att::Result<> cb_status, auto values) {
    status = cb_status;
    ASSERT_EQ(errors.size(), values.size());
    for (uint16_t i = 0; i < values.size(); i++) {
      SCOPED_TRACE(
          bt_lib_cpp_string::StringPrintf("i: %zu", static_cast<size_t>(i)));
      EXPECT_EQ(CharacteristicHandle(kStartHandle + i), values[i].handle);
      ASSERT_TRUE(values[i].result.is_error());
      EXPECT_EQ(errors[i], values[i].result.error_value());
      EXPECT_FALSE(values[i].maybe_truncated);
    }
  });

  RunUntilIdle();
  ASSERT_TRUE(status.has_value());
  ASSERT_EQ(fit::ok(), *status);
}

TEST_F(RemoteServiceManagerTest, ReadByTypeReturnsProtocolErrorAfterRead) {
  constexpr att::Handle kStartHandle = 1;
  constexpr att::Handle kEndHandle = 5;
  auto service = SetUpFakeService(ServiceData(
      ServiceKind::PRIMARY, kStartHandle, kEndHandle, kTestServiceUuid1));

  constexpr UUID kCharUuid(uint16_t{0xfefe});

  constexpr att::Handle kHandle = kEndHandle;
  const auto kValue = StaticByteBuffer(0x00, 0x01, 0x02);
  const std::vector<Client::ReadByTypeValue> kValues = {
      {kHandle, kValue.view(), /*maybe_truncated=*/false}};

  const std::vector<std::pair<const char*, att::ErrorCode>>
      general_protocol_errors = {
          {"kRequestNotSupported", att::ErrorCode::kRequestNotSupported},
          {"kInsufficientResources", att::ErrorCode::kInsufficientResources},
          {"kInvalidPDU", att::ErrorCode::kInvalidPDU}};

  for (const auto& [name, code] : general_protocol_errors) {
    SCOPED_TRACE(bt_lib_cpp_string::StringPrintf("Error Code: %s", name));
    size_t read_count = 0;
    fake_client()->set_read_by_type_request_callback(
        [&, err_code = code](
            const UUID&, att::Handle, att::Handle, auto callback) {
          ASSERT_EQ(0u, read_count++);
          switch (read_count++) {
            case 0:
              callback(fit::ok(kValues));
              break;
            case 1:
              callback(fit::error(
                  Client::ReadByTypeError{att::Error(err_code), std::nullopt}));
              break;
            default:
              FAIL();
          }
        });

    std::optional<att::Result<>> status;
    service->ReadByType(kCharUuid, [&](att::Result<> cb_status, auto values) {
      status = cb_status;
      EXPECT_EQ(0u, values.size());
    });

    RunUntilIdle();
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(ToResult(code), status);
  }
}

TEST_F(RemoteServiceManagerTest, ReadByTypeHandlesReadErrorWithMissingHandle) {
  constexpr att::Handle kStartHandle = 1;
  constexpr att::Handle kEndHandle = 5;
  auto service = SetUpFakeService(ServiceData(
      ServiceKind::PRIMARY, kStartHandle, kEndHandle, kTestServiceUuid1));

  constexpr UUID kCharUuid(uint16_t{0xfefe});

  size_t read_count = 0;
  fake_client()->set_read_by_type_request_callback(
      [&](const UUID&, att::Handle, att::Handle, auto callback) {
        ASSERT_EQ(0u, read_count++);
        callback(fit::error(Client::ReadByTypeError{
            att::Error(att::ErrorCode::kReadNotPermitted), std::nullopt}));
      });

  std::optional<att::Result<>> status;
  service->ReadByType(
      kCharUuid, [&](att::Result<> cb_status, auto) { status = cb_status; });
  RunUntilIdle();
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(ToResult(att::ErrorCode::kReadNotPermitted), *status);
}

TEST_F(RemoteServiceManagerTest,
       ReadByTypeHandlesReadErrorWithOutOfRangeHandle) {
  constexpr att::Handle kStartHandle = 1;
  constexpr att::Handle kEndHandle = 5;
  auto service = SetUpFakeService(ServiceData(
      ServiceKind::PRIMARY, kStartHandle, kEndHandle, kTestServiceUuid1));

  constexpr UUID kCharUuid(uint16_t{0xfefe});

  size_t read_count = 0;
  fake_client()->set_read_by_type_request_callback(
      [&](const UUID&, att::Handle, att::Handle, auto callback) {
        ASSERT_EQ(0u, read_count++);
        callback(fit::error(Client::ReadByTypeError{
            att::Error(att::ErrorCode::kReadNotPermitted), kEndHandle + 1}));
      });

  std::optional<att::Result<>> status;
  service->ReadByType(
      kCharUuid, [&](att::Result<> cb_status, auto) { status = cb_status; });
  RunUntilIdle();
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(ToResult(HostError::kPacketMalformed), *status);
}

TEST_F(RemoteServiceManagerTest, ReadByTypeReturnsErrorIfUuidIsInternal) {
  const std::array<UUID, 10> kInternalUuids = {
      types::kPrimaryService,
      types::kSecondaryService,
      types::kIncludeDeclaration,
      types::kCharacteristicDeclaration,
      types::kCharacteristicExtProperties,
      types::kCharacteristicUserDescription,
      types::kClientCharacteristicConfig,
      types::kServerCharacteristicConfig,
      types::kCharacteristicFormat,
      types::kCharacteristicAggregateFormat};

  auto service = SetUpFakeService(ServiceData(
      ServiceKind::PRIMARY, 1, kDefaultChrcValueHandle, kTestServiceUuid1));

  fake_client()->set_read_by_type_request_callback(
      [&](auto, auto, auto, auto) { ADD_FAILURE(); });

  for (const UUID& uuid : kInternalUuids) {
    std::optional<att::Result<>> status;
    service->ReadByType(uuid, [&](att::Result<> cb_status, auto values) {
      status = cb_status;
      EXPECT_EQ(0u, values.size());
    });

    RunUntilIdle();
    ASSERT_TRUE(status.has_value()) << "UUID: " << uuid;
    EXPECT_EQ(ToResult(HostError::kInvalidParameters), *status);
  }
}

TEST_F(RemoteServiceManagerTest, WriteCharWhileNotReady) {
  auto service = SetUpFakeService(
      ServiceData(ServiceKind::PRIMARY, 1, 2, kTestServiceUuid1));

  att::Result<> status = fit::ok();
  service->WriteCharacteristic(
      kDefaultCharacteristic,
      std::vector<uint8_t>(),
      [&](att::Result<> cb_status) { status = cb_status; });

  RunUntilIdle();

  EXPECT_EQ(ToResult(HostError::kNotReady), status);
}

TEST_F(RemoteServiceManagerTest, WriteCharNotFound) {
  auto service = SetupServiceWithChrcs(
      ServiceData(ServiceKind::PRIMARY, 1, 2, kTestServiceUuid1), {});

  att::Result<> status = fit::ok();
  service->WriteCharacteristic(
      kDefaultCharacteristic,
      std::vector<uint8_t>(),
      [&](att::Result<> cb_status) { status = cb_status; });

  RunUntilIdle();

  EXPECT_EQ(ToResult(HostError::kNotFound), status);
}

TEST_F(RemoteServiceManagerTest, WriteCharNotSupported) {
  // No "write" property set.
  auto service = SetupServiceWithChrcs(
      ServiceData(ServiceKind::PRIMARY, 1, 3, kTestServiceUuid1),
      {ReadableChrc()});

  att::Result<> status = fit::ok();
  service->WriteCharacteristic(
      kDefaultCharacteristic,
      std::vector<uint8_t>(),
      [&](att::Result<> cb_status) { status = cb_status; });

  RunUntilIdle();

  EXPECT_EQ(ToResult(HostError::kNotSupported), status);
}

TEST_F(RemoteServiceManagerTest, WriteCharSendsWriteRequest) {
  const std::vector<uint8_t> kValue{{'t', 'e', 's', 't'}};
  constexpr att::Result<> kStatus =
      ToResult(att::ErrorCode::kWriteNotPermitted);

  auto service = SetupServiceWithChrcs(
      ServiceData(
          ServiceKind::PRIMARY, 1, kDefaultChrcValueHandle, kTestServiceUuid1),
      {WritableChrc()});

  fake_client()->set_write_request_callback(
      [&](att::Handle handle, const auto& value, auto status_callback) {
        EXPECT_EQ(kDefaultChrcValueHandle, handle);
        EXPECT_TRUE(std::equal(
            kValue.begin(), kValue.end(), value.begin(), value.end()));
        status_callback(kStatus);
      });

  att::Result<> status = fit::ok();
  service->WriteCharacteristic(
      kDefaultCharacteristic, kValue, [&](att::Result<> cb_status) {
        status = cb_status;
      });

  RunUntilIdle();

  EXPECT_EQ(kStatus, status);
}

// Tests that a long write is chunked up properly into a series of QueuedWrites
// that will be processed by the client. This tests a non-zero offset.
TEST_F(RemoteServiceManagerTest, WriteCharLongOffsetSuccess) {
  constexpr uint16_t kOffset = 5;
  constexpr uint16_t kExpectedQueueSize = 4;
  constexpr uint16_t kExpectedFullWriteSize = 18;
  constexpr uint16_t kExpectedFinalWriteSize = 15;

  auto service = SetupServiceWithChrcs(
      ServiceData(
          ServiceKind::PRIMARY, 1, kDefaultChrcValueHandle, kTestServiceUuid1),
      {WritableChrc()});

  // Create a vector that will take 4 requests to write. Since the default MTU
  // is 23:
  //   a. The size of |full_write_value| is 69.
  //   b. att:Handle, |kOffset|, and att::OpCode size is 5 bytes total.
  //   c. We should write 18 + 18 + 18 + 15 bytes across 4 requests.
  //   d. These bytes will be written with offset 5, from (5) to (5+69)
  std::vector<uint8_t> full_write_value(att::kLEMinMTU * 3);

  // Initialize the contents.
  for (uint8_t i = 0; i < full_write_value.size(); ++i) {
    full_write_value[i] = i;
  }

  uint8_t process_long_write_count = 0;
  fake_client()->set_execute_prepare_writes_callback(
      [&](att::PrepareWriteQueue write_queue,
          auto /*reliable_mode*/,
          auto callback) {
        EXPECT_EQ(write_queue.size(), kExpectedQueueSize);

        for (int i = 0; i < kExpectedQueueSize; i++) {
          auto write = std::move(write_queue.front());
          write_queue.pop();

          EXPECT_EQ(write.handle(), kDefaultChrcValueHandle);
          EXPECT_EQ(write.offset(), kOffset + (i * kExpectedFullWriteSize));

          // All writes expect the final should be full, the final should be
          // the remainder.
          if (i < kExpectedQueueSize - 1) {
            EXPECT_EQ(write.value().size(), kExpectedFullWriteSize);
          } else {
            EXPECT_EQ(write.value().size(), kExpectedFinalWriteSize);
          }
        }

        process_long_write_count++;

        callback(fit::ok());
      });

  ReliableMode mode = ReliableMode::kDisabled;
  att::Result<> status = ToResult(HostError::kFailed);
  service->WriteLongCharacteristic(
      kDefaultCharacteristic,
      kOffset,
      full_write_value,
      mode,
      [&](att::Result<> cb_status) { status = cb_status; });

  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(1u, process_long_write_count);
}

TEST_F(RemoteServiceManagerTest, WriteCharLongAtExactMultipleOfMtu) {
  constexpr uint16_t kOffset = 0;
  constexpr uint16_t kExpectedQueueSize = 4;
  constexpr uint16_t kExpectedFullWriteSize = 18;

  auto service = SetupServiceWithChrcs(
      ServiceData(
          ServiceKind::PRIMARY, 1, kDefaultChrcValueHandle, kTestServiceUuid1),
      {WritableChrc()});

  // Create a vector that will take 4 requests to write. Since the default MTU
  // is 23:
  //   a. The size of |full_write_value| is 72.
  //   b. att:Handle, |kOffset|, and att::OpCode size is 5 bytes total.
  //   c. We should write 18 + 18 + 18 + 18 bytes across 4 requests.
  //   d. These bytes will be written with offset 0, from (0) to (72)
  std::vector<uint8_t> full_write_value((att::kLEMinMTU - 5) * 4);

  // Initialize the contents.
  for (uint8_t i = 0; i < full_write_value.size(); ++i) {
    full_write_value[i] = i;
  }

  uint8_t process_long_write_count = 0;
  fake_client()->set_execute_prepare_writes_callback(
      [&](att::PrepareWriteQueue write_queue,
          auto /*reliable_mode*/,
          auto callback) {
        EXPECT_EQ(write_queue.size(), kExpectedQueueSize);

        for (int i = 0; i < kExpectedQueueSize; i++) {
          auto write = std::move(write_queue.front());
          write_queue.pop();

          EXPECT_EQ(write.handle(), kDefaultChrcValueHandle);
          EXPECT_EQ(write.offset(), kOffset + (i * kExpectedFullWriteSize));

          // All writes should be full
          EXPECT_EQ(write.value().size(), kExpectedFullWriteSize);
        }

        process_long_write_count++;

        callback(fit::ok());
      });

  ReliableMode mode = ReliableMode::kDisabled;
  att::Result<> status = ToResult(HostError::kFailed);
  service->WriteLongCharacteristic(
      kDefaultCharacteristic,
      kOffset,
      full_write_value,
      mode,
      [&](att::Result<> cb_status) { status = cb_status; });

  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(1u, process_long_write_count);
}

// Writing a long characteristic with ReliableMode::Enabled should succeed.
TEST_F(RemoteServiceManagerTest, WriteCharLongReliableWrite) {
  constexpr uint16_t kOffset = 0;
  constexpr uint16_t kExpectedQueueSize = 1;

  DescriptorData fake_desc1(kDesc1, types::kCharacteristicExtProperties);
  DescriptorData fake_desc2(kDesc2, kTestUuid4);

  // The callback should be triggered once to read the value of the descriptor
  // containing the ExtendedProperties bitfield.
  auto extended_prop_read_cb = [&](att::Handle, auto callback) {
    callback(fit::ok(), kExtendedPropValue, /*maybe_truncated=*/false);
  };
  fake_client()->set_read_request_callback(std::move(extended_prop_read_cb));

  auto service = SetupServiceWithChrcs(
      ServiceData(ServiceKind::PRIMARY, kStart, kEnd, kTestServiceUuid1),
      {WriteableExtendedPropChrc()},
      {fake_desc1, fake_desc2});

  // Create a vector that will take 1 request to write. Since the default MTU
  // is 23:
  //   a. The size of |full_write_value| is 18.
  //   b. att:Handle, |kOffset|, and att::OpCode size is 5 bytes total.
  //   c. We should write 18 bytes.
  std::vector<uint8_t> full_write_value((att::kLEMinMTU - 5));

  // Initialize the contents.
  for (uint8_t i = 0; i < full_write_value.size(); ++i) {
    full_write_value[i] = i;
  }

  uint8_t process_long_write_count = 0;
  fake_client()->set_execute_prepare_writes_callback(
      [&](att::PrepareWriteQueue write_queue,
          auto /*reliable_mode*/,
          auto callback) {
        EXPECT_EQ(write_queue.size(), kExpectedQueueSize);
        process_long_write_count++;
        callback(fit::ok());
      });

  ReliableMode mode = ReliableMode::kEnabled;
  att::Result<> status = ToResult(HostError::kFailed);
  service->WriteLongCharacteristic(
      kDefaultCharacteristic,
      kOffset,
      full_write_value,
      mode,
      [&](att::Result<> cb_status) { status = cb_status; });

  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(1u, process_long_write_count);
}

TEST_F(RemoteServiceManagerTest, WriteWithoutResponseNotSupported) {
  ServiceData data(ServiceKind::PRIMARY, 1, 3, kTestServiceUuid1);
  auto service = SetUpFakeService(data);

  // No "write" or "write without response" property.
  CharacteristicData chr(0, std::nullopt, 2, 3, kTestUuid3);
  SetupCharacteristics(service, {{chr}});

  bool called = false;
  fake_client()->set_write_without_rsp_callback(
      [&](auto, const auto&, auto) { called = true; });

  std::optional<att::Result<>> status;
  service->WriteCharacteristicWithoutResponse(
      kDefaultCharacteristic,
      std::vector<uint8_t>(),
      [&](att::Result<> cb_status) { status = cb_status; });
  RunUntilIdle();
  EXPECT_FALSE(called);
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(ToResult(HostError::kNotSupported), *status);
}

TEST_F(RemoteServiceManagerTest,
       WriteWithoutResponseBeforeCharacteristicDiscovery) {
  ServiceData data(ServiceKind::PRIMARY, 1, 3, kTestServiceUuid1);
  auto service = SetUpFakeService(data);

  bool called = false;
  fake_client()->set_write_without_rsp_callback(
      [&](auto, const auto&, auto) { called = true; });

  std::optional<att::Result<>> status;
  service->WriteCharacteristicWithoutResponse(
      kDefaultCharacteristic,
      std::vector<uint8_t>(),
      [&](att::Result<> cb_status) { status = cb_status; });
  RunUntilIdle();
  EXPECT_FALSE(called);
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(ToResult(HostError::kNotReady), *status);
}

TEST_F(RemoteServiceManagerTest,
       WriteWithoutResponseSuccessWithWriteWithoutResponseProperty) {
  const std::vector<uint8_t> kValue{{'t', 'e', 's', 't'}};

  CharacteristicData chr(Property::kWriteWithoutResponse,
                         std::nullopt,
                         2,
                         kDefaultChrcValueHandle,
                         kTestUuid3);
  auto service = SetupServiceWithChrcs(
      ServiceData(
          ServiceKind::PRIMARY, 1, kDefaultChrcValueHandle, kTestServiceUuid1),
      {chr});

  bool called = false;
  fake_client()->set_write_without_rsp_callback(
      [&](att::Handle handle, const auto& value, att::ResultFunction<> cb) {
        EXPECT_EQ(kDefaultChrcValueHandle, handle);
        EXPECT_TRUE(std::equal(
            kValue.begin(), kValue.end(), value.begin(), value.end()));
        called = true;
        cb(fit::ok());
      });

  std::optional<att::Result<>> status;
  service->WriteCharacteristicWithoutResponse(
      kDefaultCharacteristic, kValue, [&](att::Result<> cb_status) {
        status = cb_status;
      });
  RunUntilIdle();
  EXPECT_TRUE(called);
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(fit::ok(), *status);
}

TEST_F(RemoteServiceManagerTest, WriteWithoutResponseSuccessWithWriteProperty) {
  const std::vector<uint8_t> kValue{{'t', 'e', 's', 't'}};

  CharacteristicData chr(
      Property::kWrite, std::nullopt, 2, kDefaultChrcValueHandle, kTestUuid3);
  auto service = SetupServiceWithChrcs(
      ServiceData(
          ServiceKind::PRIMARY, 1, kDefaultChrcValueHandle, kTestServiceUuid1),
      {chr});

  bool called = false;
  fake_client()->set_write_without_rsp_callback(
      [&](att::Handle handle, const auto& value, att::ResultFunction<> cb) {
        EXPECT_EQ(kDefaultChrcValueHandle, handle);
        EXPECT_TRUE(std::equal(
            kValue.begin(), kValue.end(), value.begin(), value.end()));
        called = true;
        cb(fit::ok());
      });

  std::optional<att::Result<>> status;
  service->WriteCharacteristicWithoutResponse(
      kDefaultCharacteristic, kValue, [&](att::Result<> cb_status) {
        status = cb_status;
      });
  RunUntilIdle();

  EXPECT_TRUE(called);
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(fit::ok(), *status);
}

TEST_F(RemoteServiceManagerTest, ReadDescWhileNotReady) {
  auto service = SetUpFakeService(
      ServiceData(ServiceKind::PRIMARY, 1, 2, kTestServiceUuid1));

  att::Result<> status = fit::ok();
  service->ReadDescriptor(0, [&](att::Result<> cb_status, const auto&, auto) {
    status = cb_status;
  });

  RunUntilIdle();

  EXPECT_EQ(ToResult(HostError::kNotReady), status);
}

TEST_F(RemoteServiceManagerTest, ReadDescriptorNotFound) {
  auto service = SetupServiceWithChrcs(
      ServiceData(ServiceKind::PRIMARY, 1, 2, kTestServiceUuid1), {});

  att::Result<> status = fit::ok();
  service->ReadDescriptor(0, [&](att::Result<> cb_status, const auto&, auto) {
    status = cb_status;
  });

  RunUntilIdle();

  EXPECT_EQ(ToResult(HostError::kNotFound), status);
}

TEST_F(RemoteServiceManagerTest, ReadDescSendsReadRequest) {
  // TODO(armansito): Some of the service set up and |status| verification
  // boilerplate could be reduced by factoring them out into helpers on the test
  // harness (also see code review comment in
  // https://fuchsia-review.googlesource.com/c/garnet/+/213794/6/drivers/bluetooth/lib/gatt/remote_service_manager_test.cc).
  constexpr att::Handle kValueHandle1 = 3;
  constexpr att::Handle kValueHandle2 = 5;
  constexpr att::Handle kDescrHandle = 6;

  ServiceData data(ServiceKind::PRIMARY, 1, kDescrHandle, kTestServiceUuid1);
  auto service = SetUpFakeService(data);

  CharacteristicData chr1(
      Property::kRead, std::nullopt, 2, kValueHandle1, kTestUuid3);
  CharacteristicData chr2(
      Property::kRead, std::nullopt, 4, kValueHandle2, kTestUuid3);
  DescriptorData desc(kDescrHandle, kTestUuid4);
  SetupCharacteristics(service, {{chr1, chr2}}, {{desc}});

  const StaticByteBuffer kValue('t', 'e', 's', 't');

  fake_client()->set_read_request_callback(
      [&](att::Handle handle, auto callback) {
        EXPECT_EQ(kDescrHandle, handle);
        callback(fit::ok(), kValue, /*maybe_truncated=*/false);
      });

  att::Result<> status = ToResult(HostError::kFailed);
  service->ReadDescriptor(
      DescriptorHandle(kDescrHandle),
      [&](att::Result<> cb_status, const auto& value, auto) {
        status = cb_status;
        EXPECT_TRUE(ContainersEqual(kValue, value));
      });

  RunUntilIdle();

  EXPECT_EQ(fit::ok(), status);
}

TEST_F(RemoteServiceManagerTest, ReadLongDescWhileNotReady) {
  auto service = SetUpFakeService(
      ServiceData(ServiceKind::PRIMARY, 1, 2, kTestServiceUuid1));

  att::Result<> status = fit::ok();
  service->ReadLongDescriptor(
      0, 0, 512, [&](att::Result<> cb_status, const auto&, auto) {
        status = cb_status;
      });

  RunUntilIdle();

  EXPECT_EQ(ToResult(HostError::kNotReady), status);
}

TEST_F(RemoteServiceManagerTest, ReadLongDescNotFound) {
  auto service = SetupServiceWithChrcs(
      ServiceData(ServiceKind::PRIMARY, 1, 2, kTestServiceUuid1), {});

  att::Result<> status = fit::ok();
  service->ReadLongDescriptor(
      0, 0, 512, [&](att::Result<> cb_status, const auto&, auto) {
        status = cb_status;
      });

  RunUntilIdle();

  EXPECT_EQ(ToResult(HostError::kNotFound), status);
}

// Tests that ReadLongDescriptor sends Read Blob requests. Other conditions
// around the long read procedure are already covered by the tests for
// ReadLongCharacteristic as the implementations are shared.
TEST_F(RemoteServiceManagerTest, ReadLongDescriptor) {
  constexpr att::Handle kValueHandle = 3;
  constexpr att::Handle kDescrHandle = 4;
  constexpr uint16_t kOffset = 0;
  constexpr size_t kMaxBytes = 1000;
  constexpr int kExpectedBlobCount = 4;

  ServiceData data(ServiceKind::PRIMARY, 1, kDescrHandle, kTestServiceUuid1);
  auto service = SetUpFakeService(data);

  CharacteristicData chr(
      Property::kRead, std::nullopt, 2, kValueHandle, kTestUuid3);
  DescriptorData desc(kDescrHandle, kTestUuid4);
  SetupCharacteristics(service, {{chr}}, {{desc}});

  // Create a buffer that will take 4 requests to read. Since the default MTU is
  // 23:
  //   a. The size of |expected_value| is 69.
  //   b. We should read 22 + 22 + 22 + 3 bytes across 4 requests.
  StaticByteBuffer<att::kLEMinMTU * 3> expected_value;

  // Initialize the contents.
  for (uint8_t i = 0; i < expected_value.size(); ++i) {
    expected_value[i] = i;
  }

  int read_count = 0;
  fake_client()->set_read_request_callback(
      [&](att::Handle handle, auto callback) {
        read_count++;
        EXPECT_EQ(read_count, 1);
        EXPECT_EQ(kDescrHandle, handle);
        auto blob = expected_value.view(0, att::kLEMinMTU - 1);
        callback(fit::ok(), blob, /*maybe_truncated=*/true);
      });

  fake_client()->set_read_blob_request_callback(
      [&](att::Handle handle, uint16_t offset, auto callback) {
        read_count++;
        EXPECT_GT(read_count, 1);
        EXPECT_EQ(kDescrHandle, handle);
        bool maybe_truncated = true;

        // Return a blob at the given offset with at most MTU - 1 bytes.
        auto blob = expected_value.view(offset, att::kLEMinMTU - 1);
        if (read_count == kExpectedBlobCount) {
          // The final blob should contain 3 bytes.
          EXPECT_EQ(3u, blob.size());
          maybe_truncated = false;
        }

        callback(fit::ok(), blob, maybe_truncated);
      });

  att::Result<> status = ToResult(HostError::kFailed);
  service->ReadLongDescriptor(
      DescriptorHandle(kDescrHandle),
      kOffset,
      kMaxBytes,
      [&](att::Result<> cb_status, const auto& value, bool maybe_truncated) {
        status = cb_status;
        EXPECT_TRUE(ContainersEqual(expected_value, value));
        EXPECT_FALSE(maybe_truncated);
      });

  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(kExpectedBlobCount, read_count);
}

TEST_F(RemoteServiceManagerTest, WriteDescWhileNotReady) {
  auto service = SetUpFakeService(
      ServiceData(ServiceKind::PRIMARY, 1, 2, kTestServiceUuid1));

  att::Result<> status = fit::ok();
  service->WriteDescriptor(
      0, std::vector<uint8_t>(), [&](att::Result<> cb_status) {
        status = cb_status;
      });

  RunUntilIdle();

  EXPECT_EQ(ToResult(HostError::kNotReady), status);
}

TEST_F(RemoteServiceManagerTest, WriteDescNotFound) {
  auto service = SetUpFakeService(
      ServiceData(ServiceKind::PRIMARY, 1, 2, kTestServiceUuid1));
  SetupCharacteristics(service, std::vector<CharacteristicData>());

  att::Result<> status = fit::ok();
  service->WriteDescriptor(
      0, std::vector<uint8_t>(), [&](att::Result<> cb_status) {
        status = cb_status;
      });

  RunUntilIdle();

  EXPECT_EQ(ToResult(HostError::kNotFound), status);
}

TEST_F(RemoteServiceManagerTest, WriteDescNotAllowed) {
  auto service = SetUpFakeService(
      ServiceData(ServiceKind::PRIMARY, 1, 4, kTestServiceUuid1));

  // "CCC" characteristic cannot be written to.
  CharacteristicData chr(0, std::nullopt, 2, 3, kTestUuid3);
  DescriptorData desc(4, types::kClientCharacteristicConfig);
  SetupCharacteristics(service, {{chr}}, {{desc}});

  att::Result<> status = fit::ok();
  service->WriteDescriptor(
      4, std::vector<uint8_t>(), [&](att::Result<> cb_status) {
        status = cb_status;
      });

  RunUntilIdle();

  EXPECT_EQ(ToResult(HostError::kNotSupported), status);
}

TEST_F(RemoteServiceManagerTest, WriteDescSendsWriteRequest) {
  constexpr att::Handle kValueHandle = 3;
  constexpr att::Handle kDescrHandle = 4;
  const std::vector<uint8_t> kValue{{'t', 'e', 's', 't'}};
  const att::Result<> kStatus = ToResult(HostError::kNotSupported);

  ServiceData data(ServiceKind::PRIMARY, 1, kDescrHandle, kTestServiceUuid1);
  auto service = SetUpFakeService(data);

  CharacteristicData chr(
      Property::kWrite, std::nullopt, 2, kValueHandle, kTestUuid3);
  DescriptorData desc(kDescrHandle, kTestUuid4);
  SetupCharacteristics(service, {{chr}}, {{desc}});

  fake_client()->set_write_request_callback(
      [&](att::Handle handle, const auto& value, auto status_callback) {
        EXPECT_EQ(kDescrHandle, handle);
        EXPECT_TRUE(std::equal(
            kValue.begin(), kValue.end(), value.begin(), value.end()));
        status_callback(kStatus);
      });

  att::Result<> status = fit::ok();
  service->WriteDescriptor(kDescrHandle, kValue, [&](att::Result<> cb_status) {
    status = cb_status;
  });

  RunUntilIdle();
  EXPECT_EQ(kStatus, status);
}

// Tests that WriteDescriptor with a long vector is prepared correctly.
// Other conditions around the long write procedure are already covered by the
// tests for WriteCharacteristic as the implementations are shared.
TEST_F(RemoteServiceManagerTest, WriteDescLongSuccess) {
  constexpr att::Handle kValueHandle = 3;
  constexpr att::Handle kDescrHandle = 4;
  constexpr uint16_t kOffset = 0;
  constexpr uint16_t kExpectedQueueSize = 4;
  constexpr uint16_t kExpectedFullWriteSize = 18;
  constexpr uint16_t kExpectedFinalWriteSize = 15;

  ServiceData data(ServiceKind::PRIMARY, 1, kDescrHandle, kTestServiceUuid1);
  auto service = SetUpFakeService(data);

  CharacteristicData chr(
      Property::kWrite, std::nullopt, 2, kValueHandle, kTestUuid3);
  DescriptorData desc(kDescrHandle, kTestUuid4);
  SetupCharacteristics(service, {{chr}}, {{desc}});

  // Create a vector that will take 4 requests to write. Since the default MTU
  // is 23:
  //   a. The size of |full_write_value| is 69.
  //   b. att:Handle, |kOffset|, and att::OpCode size is 5 bytes total.
  //   c. We should write 18 + 18 + 18 + 15 bytes across 4 requests.
  //   d. These bytes will be written with offset 5, from (5) to (5+69)
  std::vector<uint8_t> full_write_value(att::kLEMinMTU * 3);

  // Initialize the contents.
  for (uint8_t i = 0; i < full_write_value.size(); ++i) {
    full_write_value[i] = i;
  }

  uint8_t process_long_write_count = 0;
  fake_client()->set_execute_prepare_writes_callback(
      [&](att::PrepareWriteQueue write_queue,
          auto /*reliable_mode*/,
          auto callback) {
        EXPECT_EQ(write_queue.size(), kExpectedQueueSize);

        att::QueuedWrite prepare_write;
        for (int i = 0; i < kExpectedQueueSize; i++) {
          auto write = std::move(write_queue.front());
          write_queue.pop();

          EXPECT_EQ(write.handle(), kDescrHandle);
          EXPECT_EQ(write.offset(), kOffset + (i * kExpectedFullWriteSize));

          // All writes expect the final should be full, the final should be
          // the remainder.
          if (i < kExpectedQueueSize - 1) {
            EXPECT_EQ(write.value().size(), kExpectedFullWriteSize);
          } else {
            EXPECT_EQ(write.value().size(), kExpectedFinalWriteSize);
          }
        }

        process_long_write_count++;

        callback(fit::ok());
      });

  att::Result<> status = ToResult(HostError::kFailed);
  service->WriteLongDescriptor(
      DescriptorHandle(kDescrHandle),
      kOffset,
      full_write_value,
      [&](att::Result<> cb_status) { status = cb_status; });

  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(1u, process_long_write_count);
}

TEST_F(RemoteServiceManagerTest, EnableNotificationsWhileNotReady) {
  auto service = SetUpFakeService(
      ServiceData(ServiceKind::PRIMARY, 1, 2, kTestServiceUuid1));

  att::Result<> status = fit::ok();
  service->EnableNotifications(
      kDefaultCharacteristic,
      NopValueCallback,
      [&](att::Result<> cb_status, IdType) { status = cb_status; });

  RunUntilIdle();

  EXPECT_EQ(ToResult(HostError::kNotReady), status);
}

TEST_F(RemoteServiceManagerTest, EnableNotificationsCharNotFound) {
  auto service = SetupServiceWithChrcs(
      ServiceData(ServiceKind::PRIMARY, 1, 2, kTestServiceUuid1), {});

  att::Result<> status = fit::ok();
  service->EnableNotifications(
      kDefaultCharacteristic,
      NopValueCallback,
      [&](att::Result<> cb_status, IdType) { status = cb_status; });

  RunUntilIdle();

  EXPECT_EQ(ToResult(HostError::kNotFound), status);
}

TEST_F(RemoteServiceManagerTest, EnableNotificationsNoProperties) {
  auto service = SetUpFakeService(
      ServiceData(ServiceKind::PRIMARY, 1, 4, kTestServiceUuid1));

  // Has neither the "notify" nor "indicate" property but has a CCC descriptor.
  CharacteristicData chr(Property::kRead, std::nullopt, 2, 3, kTestUuid3);
  DescriptorData desc(4, types::kClientCharacteristicConfig);
  SetupCharacteristics(service, {{chr}}, {{desc}});

  att::Result<> status = fit::ok();
  service->EnableNotifications(
      kDefaultCharacteristic,
      NopValueCallback,
      [&](att::Result<> cb_status, IdType) { status = cb_status; });
  RunUntilIdle();

  EXPECT_EQ(ToResult(HostError::kNotSupported), status);
}

TEST_F(RemoteServiceManagerTest, EnableNotificationsSuccess) {
  constexpr att::Handle kCCCHandle = 4;
  auto service = SetUpFakeService(
      ServiceData(ServiceKind::PRIMARY, 1, kCCCHandle, kTestServiceUuid1));

  CharacteristicData chr(Property::kNotify, std::nullopt, 2, 3, kTestUuid3);
  DescriptorData desc(kCCCHandle, types::kClientCharacteristicConfig);
  SetupCharacteristics(service, {{chr}}, {{desc}});

  fake_client()->set_write_request_callback(
      [&](att::Handle handle, const auto& value, auto status_callback) {
        EXPECT_EQ(kCCCHandle, handle);
        EXPECT_TRUE(ContainersEqual(kCCCNotifyValue, value));
        status_callback(fit::ok());
      });

  IdType id = kInvalidId;
  att::Result<> status = ToResult(HostError::kFailed);
  service->EnableNotifications(kDefaultCharacteristic,
                               NopValueCallback,
                               [&](att::Result<> cb_status, IdType cb_id) {
                                 status = cb_status;
                                 id = cb_id;
                               });

  RunUntilIdle();

  EXPECT_EQ(fit::ok(), status);
  EXPECT_NE(kInvalidId, id);
}

TEST_F(RemoteServiceManagerTest, EnableIndications) {
  constexpr att::Handle kCCCHandle = 4;
  auto service = SetUpFakeService(
      ServiceData(ServiceKind::PRIMARY, 1, kCCCHandle, kTestServiceUuid1));

  CharacteristicData chr(Property::kIndicate, std::nullopt, 2, 3, kTestUuid3);
  DescriptorData desc(kCCCHandle, types::kClientCharacteristicConfig);
  SetupCharacteristics(service, {{chr}}, {{desc}});

  fake_client()->set_write_request_callback(
      [&](att::Handle handle, const auto& value, auto status_callback) {
        EXPECT_EQ(kCCCHandle, handle);
        EXPECT_TRUE(ContainersEqual(kCCCIndicateValue, value));
        status_callback(fit::ok());
      });

  IdType id = kInvalidId;
  att::Result<> status = ToResult(HostError::kFailed);
  service->EnableNotifications(kDefaultCharacteristic,
                               NopValueCallback,
                               [&](att::Result<> cb_status, IdType cb_id) {
                                 status = cb_status;
                                 id = cb_id;
                               });

  RunUntilIdle();

  EXPECT_EQ(fit::ok(), status);
  EXPECT_NE(kInvalidId, id);
}

TEST_F(RemoteServiceManagerTest, EnableNotificationsError) {
  constexpr att::Handle kCCCHandle = 4;

  auto service = SetUpFakeService(
      ServiceData(ServiceKind::PRIMARY, 1, 4, kTestServiceUuid1));

  CharacteristicData chr(Property::kNotify, std::nullopt, 2, 3, kTestUuid3);
  DescriptorData desc(kCCCHandle, types::kClientCharacteristicConfig);
  SetupCharacteristics(service, {{chr}}, {{desc}});

  // Should enable notifications
  const StaticByteBuffer kExpectedValue(0x01, 0x00);

  fake_client()->set_write_request_callback(
      [&](att::Handle handle, const auto& value, auto status_callback) {
        EXPECT_EQ(kCCCHandle, handle);
        EXPECT_TRUE(ContainersEqual(kExpectedValue, value));
        status_callback(ToResult(att::ErrorCode::kUnlikelyError));
      });

  IdType id = kInvalidId;
  att::Result<> status = fit::ok();
  service->EnableNotifications(kDefaultCharacteristic,
                               NopValueCallback,
                               [&](att::Result<> cb_status, IdType cb_id) {
                                 status = cb_status;
                                 id = cb_id;
                               });

  RunUntilIdle();

  EXPECT_EQ(ToResult(att::ErrorCode::kUnlikelyError), status);
  EXPECT_EQ(kInvalidId, id);
}

TEST_F(RemoteServiceManagerTest, EnableNotificationsRequestMany) {
  constexpr att::Handle kCCCHandle1 = 4;
  constexpr att::Handle kCCCHandle2 = 7;

  auto service = SetUpFakeService(
      ServiceData(ServiceKind::PRIMARY, 1, 7, kTestServiceUuid1));

  // Set up two characteristics
  CharacteristicData chr1(Property::kNotify, std::nullopt, 2, 3, kTestUuid3);
  DescriptorData desc1(kCCCHandle1, types::kClientCharacteristicConfig);

  CharacteristicData chr2(Property::kIndicate, std::nullopt, 5, 6, kTestUuid3);
  DescriptorData desc2(kCCCHandle2, types::kClientCharacteristicConfig);

  SetupCharacteristics(service, {{chr1, chr2}}, {{desc1, desc2}});

  int ccc_write_count = 0;
  att::ResultFunction<> status_callback1, status_callback2;
  fake_client()->set_write_request_callback(
      [&](att::Handle handle, const auto& value, auto status_cb) {
        if (handle == kCCCHandle1) {
          EXPECT_TRUE(ContainersEqual(kCCCNotifyValue, value));
          status_callback1 = std::move(status_cb);
        } else if (handle == kCCCHandle2) {
          EXPECT_TRUE(ContainersEqual(kCCCIndicateValue, value));
          status_callback2 = std::move(status_cb);
        } else {
          ADD_FAILURE() << "Unexpected handle: " << handle;
        }
        ccc_write_count++;
      });

  size_t cb_count = 0u;

  service->EnableNotifications(kDefaultCharacteristic,
                               NopValueCallback,
                               [&](att::Result<> status, IdType id) {
                                 cb_count++;
                                 EXPECT_EQ(1u, id);
                                 EXPECT_EQ(fit::ok(), status);
                               });
  service->EnableNotifications(kDefaultCharacteristic,
                               NopValueCallback,
                               [&](att::Result<> status, IdType id) {
                                 cb_count++;
                                 EXPECT_EQ(2u, id);
                                 EXPECT_EQ(fit::ok(), status);
                               });
  service->EnableNotifications(kSecondCharacteristic,
                               NopValueCallback,
                               [&](att::Result<> status, IdType id) {
                                 cb_count++;
                                 EXPECT_EQ(1u, id);
                                 EXPECT_EQ(fit::ok(), status);
                               });
  service->EnableNotifications(kSecondCharacteristic,
                               NopValueCallback,
                               [&](att::Result<> status, IdType id) {
                                 cb_count++;
                                 EXPECT_EQ(2u, id);
                                 EXPECT_EQ(fit::ok(), status);
                               });
  service->EnableNotifications(kSecondCharacteristic,
                               NopValueCallback,
                               [&](att::Result<> status, IdType id) {
                                 cb_count++;
                                 EXPECT_EQ(3u, id);
                                 EXPECT_EQ(fit::ok(), status);
                               });

  RunUntilIdle();

  // ATT write requests should be sent but none of the notification requests
  // should be resolved.
  EXPECT_EQ(2, ccc_write_count);
  EXPECT_EQ(0u, cb_count);

  // An ATT response should resolve all pending requests for the right
  // characteristic.
  status_callback1(fit::ok());
  EXPECT_EQ(2u, cb_count);
  status_callback2(fit::ok());
  EXPECT_EQ(5u, cb_count);

  // An extra request should succeed without sending any PDUs.
  service->EnableNotifications(kDefaultCharacteristic,
                               NopValueCallback,
                               [&](att::Result<> status, IdType) {
                                 cb_count++;
                                 EXPECT_EQ(fit::ok(), status);
                               });

  RunUntilIdle();

  EXPECT_EQ(2, ccc_write_count);
  EXPECT_EQ(6u, cb_count);
}

TEST_F(RemoteServiceManagerTest, EnableNotificationsRequestManyError) {
  constexpr att::Handle kCCCHandle = 4;

  auto service = SetUpFakeService(
      ServiceData(ServiceKind::PRIMARY, 1, 4, kTestServiceUuid1));

  // Set up two characteristics
  CharacteristicData chr(Property::kNotify, std::nullopt, 2, 3, kTestUuid3);
  DescriptorData desc(kCCCHandle, types::kClientCharacteristicConfig);

  SetupCharacteristics(service, {{chr}}, {{desc}});

  int ccc_write_count = 0;
  att::ResultFunction<> status_callback;
  fake_client()->set_write_request_callback(
      [&](att::Handle handle, const auto& value, auto status_cb) {
        EXPECT_EQ(kCCCHandle, handle);
        EXPECT_TRUE(ContainersEqual(kCCCNotifyValue, value));

        ccc_write_count++;
        status_callback = std::move(status_cb);
      });

  int cb_count = 0;
  att::Result<> status = fit::ok();
  auto cb = [&](att::Result<> cb_status, IdType) {
    status = cb_status;
    cb_count++;
  };

  service->EnableNotifications(
      kDefaultCharacteristic, NopValueCallback, std::move(cb));
  service->EnableNotifications(
      kDefaultCharacteristic, NopValueCallback, std::move(cb));
  service->EnableNotifications(
      kDefaultCharacteristic, NopValueCallback, std::move(cb));

  RunUntilIdle();

  // Requests should be buffered and only one ATT request should have been sent
  // out.
  EXPECT_EQ(1, ccc_write_count);
  EXPECT_EQ(0, cb_count);

  status_callback(ToResult(HostError::kNotSupported));
  EXPECT_EQ(3, cb_count);
  EXPECT_EQ(ToResult(HostError::kNotSupported), status);

  // A new request should write to the descriptor again.
  service->EnableNotifications(
      kDefaultCharacteristic, NopValueCallback, std::move(cb));

  RunUntilIdle();

  EXPECT_EQ(2, ccc_write_count);
  EXPECT_EQ(3, cb_count);

  status_callback(fit::ok());
  EXPECT_EQ(2, ccc_write_count);
  EXPECT_EQ(4, cb_count);
  EXPECT_EQ(fit::ok(), status);
}

// Enabling notifications should succeed without a descriptor write.
TEST_F(RemoteServiceManagerTest, EnableNotificationsWithoutCCC) {
  // Has the "notify" property but no CCC descriptor.
  CharacteristicData chr(Property::kNotify, std::nullopt, 2, 3, kTestUuid3);
  auto service = SetupServiceWithChrcs(
      ServiceData(ServiceKind::PRIMARY, 1, 3, kTestServiceUuid1), {chr});

  bool write_requested = false;
  fake_client()->set_write_request_callback(
      [&](auto, auto&, auto) { write_requested = true; });

  int notify_count = 0;
  auto notify_cb = [&](const auto& /*value*/, bool /*maybe_truncated*/) {
    notify_count++;
  };

  att::Result<> status = fit::ok();
  IdType id;
  service->EnableNotifications(kDefaultCharacteristic,
                               std::move(notify_cb),
                               [&](att::Result<> _status, IdType _id) {
                                 status = _status;
                                 id = _id;
                               });
  RunUntilIdle();

  EXPECT_EQ(fit::ok(), status);
  EXPECT_FALSE(write_requested);

  fake_client()->SendNotification(/*indicate=*/false,
                                  3,
                                  StaticByteBuffer('y', 'e'),
                                  /*maybe_truncated=*/false);
  EXPECT_EQ(1, notify_count);

  // Disabling notifications should not result in a write request.
  service->DisableNotifications(
      kDefaultCharacteristic, id, [&](auto _status) { status = _status; });
  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_FALSE(write_requested);

  // The handler should no longer receive notifications.
  fake_client()->SendNotification(/*indicate=*/false,
                                  3,
                                  StaticByteBuffer('o', 'y', 'e'),
                                  /*maybe_truncated=*/false);
  EXPECT_EQ(1, notify_count);
}

// Notifications received when the remote service database is empty should be
// dropped and not cause a crash.
TEST_F(RemoteServiceManagerTest, NotificationWithoutServices) {
  for (att::Handle i = 0; i < 10; ++i) {
    fake_client()->SendNotification(
        /*indicate=*/false,
        i,
        StaticByteBuffer('n', 'o', 't', 'i', 'f', 'y'),
        /*maybe_truncated=*/false);
  }
  RunUntilIdle();
}

TEST_F(RemoteServiceManagerTest, NotificationCallback) {
  auto service = SetUpFakeService(
      ServiceData(ServiceKind::PRIMARY, 1, 7, kTestServiceUuid1));

  // Set up two characteristics
  CharacteristicData chr1(Property::kNotify, std::nullopt, 2, 3, kTestUuid3);
  DescriptorData desc1(4, types::kClientCharacteristicConfig);

  CharacteristicData chr2(Property::kIndicate, std::nullopt, 5, 6, kTestUuid3);
  DescriptorData desc2(7, types::kClientCharacteristicConfig);

  SetupCharacteristics(service, {{chr1, chr2}}, {{desc1, desc2}});

  fake_client()->set_write_request_callback(
      [&](att::Handle, const auto&, auto status_callback) {
        status_callback(fit::ok());
      });

  IdType handler_id = kInvalidId;
  att::Result<> status = ToResult(HostError::kFailed);

  int chr1_count = 0;
  auto chr1_cb = [&](const ByteBuffer& value, bool maybe_truncated) {
    chr1_count++;
    EXPECT_EQ("notify", value.AsString());
    EXPECT_FALSE(maybe_truncated);
  };

  int chr2_count = 0;
  auto chr2_cb = [&](const ByteBuffer& value, bool maybe_truncated) {
    chr2_count++;
    EXPECT_EQ("indicate", value.AsString());
    EXPECT_TRUE(maybe_truncated);
  };

  // Notify both characteristics which should get dropped.
  fake_client()->SendNotification(
      /*indicate=*/false,
      3,
      StaticByteBuffer('n', 'o', 't', 'i', 'f', 'y'),
      /*maybe_truncated=*/false);
  fake_client()->SendNotification(
      /*indicate=*/true,
      6,
      StaticByteBuffer('i', 'n', 'd', 'i', 'c', 'a', 't', 'e'),
      /*maybe_truncated=*/true);

  EnableNotifications(service,
                      kDefaultCharacteristic,
                      &status,
                      &handler_id,
                      std::move(chr1_cb));
  ASSERT_EQ(fit::ok(), status);
  EnableNotifications(
      service, kSecondCharacteristic, &status, &handler_id, std::move(chr2_cb));
  ASSERT_EQ(fit::ok(), status);

  // Notify characteristic 1.
  fake_client()->SendNotification(
      /*indicate=*/false,
      3,
      StaticByteBuffer('n', 'o', 't', 'i', 'f', 'y'),
      /*maybe_truncated=*/false);
  EXPECT_EQ(1, chr1_count);
  EXPECT_EQ(0, chr2_count);

  // Notify characteristic 2.
  fake_client()->SendNotification(
      /*indicate=*/true,
      6,
      StaticByteBuffer('i', 'n', 'd', 'i', 'c', 'a', 't', 'e'),
      /*maybe_truncated=*/true);
  EXPECT_EQ(1, chr1_count);
  EXPECT_EQ(1, chr2_count);

  // Disable notifications from characteristic 1.
  status = ToResult(HostError::kFailed);
  service->DisableNotifications(
      kDefaultCharacteristic, handler_id, [&](att::Result<> cb_status) {
        status = cb_status;
      });

  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);

  // Notifications for characteristic 1 should get dropped.
  fake_client()->SendNotification(
      /*indicate=*/false,
      3,
      StaticByteBuffer('n', 'o', 't', 'i', 'f', 'y'),
      /*maybe_truncated=*/false);
  fake_client()->SendNotification(
      /*indicate=*/true,
      6,
      StaticByteBuffer('i', 'n', 'd', 'i', 'c', 'a', 't', 'e'),
      /*maybe_truncated=*/true);
  EXPECT_EQ(1, chr1_count);
  EXPECT_EQ(2, chr2_count);
}

TEST_F(RemoteServiceManagerTest, DisableNotificationsWhileNotReady) {
  ServiceData data(ServiceKind::PRIMARY, 1, 4, kTestServiceUuid1);
  auto service = SetUpFakeService(data);

  att::Result<> status = fit::ok();
  service->DisableNotifications(
      kDefaultCharacteristic, 1, [&](att::Result<> cb_status) {
        status = cb_status;
      });

  RunUntilIdle();

  EXPECT_EQ(ToResult(HostError::kNotReady), status);
}

TEST_F(RemoteServiceManagerTest, DisableNotificationsCharNotFound) {
  auto service = SetupNotifiableService();

  IdType id = kInvalidId;
  att::Result<> status = ToResult(HostError::kFailed);
  EnableNotifications(service, kDefaultCharacteristic, &status, &id);

  // "1" is an invalid characteristic ID.
  service->DisableNotifications(
      kInvalidCharacteristic, id, [&](att::Result<> cb_status) {
        status = cb_status;
      });

  RunUntilIdle();

  EXPECT_EQ(ToResult(HostError::kNotFound), status);
}

TEST_F(RemoteServiceManagerTest, DisableNotificationsIdNotFound) {
  auto service = SetupNotifiableService();

  IdType id = kInvalidId;
  att::Result<> status = ToResult(HostError::kFailed);
  EnableNotifications(service, kDefaultCharacteristic, &status, &id);

  // Valid characteristic ID but invalid notification handler ID.
  service->DisableNotifications(
      kDefaultCharacteristic, id + 1, [&](att::Result<> cb_status) {
        status = cb_status;
      });

  RunUntilIdle();

  EXPECT_EQ(ToResult(HostError::kNotFound), status);
}

TEST_F(RemoteServiceManagerTest, DisableNotificationsSingleHandler) {
  constexpr att::Handle kCCCHandle = 4;
  auto service = SetupNotifiableService();

  IdType id = kInvalidId;
  att::Result<> status = ToResult(HostError::kFailed);
  EnableNotifications(service, kDefaultCharacteristic, &status, &id);

  // Should disable notifications
  const StaticByteBuffer kExpectedValue(0x00, 0x00);

  int ccc_write_count = 0;
  fake_client()->set_write_request_callback(
      [&](att::Handle handle, const auto& value, auto status_callback) {
        EXPECT_EQ(kCCCHandle, handle);
        EXPECT_TRUE(ContainersEqual(kExpectedValue, value));
        ccc_write_count++;
        status_callback(fit::ok());
      });

  status = ToResult(HostError::kFailed);
  service->DisableNotifications(
      kDefaultCharacteristic, id, [&](att::Result<> cb_status) {
        status = cb_status;
      });

  RunUntilIdle();

  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(1, ccc_write_count);
}

TEST_F(RemoteServiceManagerTest, DisableNotificationsOnDestruction) {
  constexpr att::Handle kCCCHandle = 4;
  auto service = SetupNotifiableService();

  IdType id = kInvalidId;
  att::Result<> status = ToResult(HostError::kFailed);
  EnableNotifications(service, kDefaultCharacteristic, &status, &id);
  ASSERT_EQ(fit::ok(), status);

  // Should disable notifications
  const StaticByteBuffer kExpectedValue(0x00, 0x00);

  int ccc_write_count = 0;
  fake_client()->set_write_request_callback(
      [&](att::Handle handle, const auto& value, auto status_callback) {
        EXPECT_EQ(kCCCHandle, handle);
        EXPECT_TRUE(ContainersEqual(kExpectedValue, value));
        ccc_write_count++;
        status_callback(fit::ok());
      });

  // Destroying the service manager (which destroys the service characteristics)
  // should clear the CCC.
  DestroyServiceManager();
  RunUntilIdle();
  EXPECT_EQ(1, ccc_write_count);
}

TEST_F(RemoteServiceManagerTest, DisableNotificationsManyHandlers) {
  auto service = SetupNotifiableService();

  IdType id = kInvalidId;
  std::vector<IdType> handler_ids;

  for (int i = 0; i < 2; i++) {
    att::Result<> status = ToResult(HostError::kFailed);
    EnableNotifications(service, kDefaultCharacteristic, &status, &id);
    ASSERT_EQ(fit::ok(), status);
    handler_ids.push_back(id);
  }

  int ccc_write_count = 0;
  fake_client()->set_write_request_callback(
      [&](att::Handle, const auto&, auto status_callback) {
        ccc_write_count++;
        status_callback(fit::ok());
      });

  // Disabling should succeed without an ATT transaction.
  att::Result<> status = ToResult(HostError::kFailed);
  service->DisableNotifications(
      kDefaultCharacteristic, handler_ids.back(), [&](att::Result<> cb_status) {
        status = cb_status;
      });
  handler_ids.pop_back();
  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(0, ccc_write_count);

  // Enabling should succeed without an ATT transaction.
  status = ToResult(HostError::kFailed);
  EnableNotifications(service, kDefaultCharacteristic, &status, &id);
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(0, ccc_write_count);
  handler_ids.push_back(id);

  // Disabling all should send out an ATT transaction.
  while (!handler_ids.empty()) {
    status = ToResult(HostError::kFailed);
    service->DisableNotifications(
        kDefaultCharacteristic,
        handler_ids.back(),
        [&](att::Result<> cb_status) { status = cb_status; });
    handler_ids.pop_back();
    RunUntilIdle();
    EXPECT_EQ(fit::ok(), status);
  }

  EXPECT_EQ(1, ccc_write_count);
}

TEST_F(RemoteServiceManagerTest,
       ReadByTypeErrorOnLastHandleDoesNotOverflowHandle) {
  constexpr att::Handle kStartHandle = 0xFFFE;
  constexpr att::Handle kEndHandle = 0xFFFF;
  auto service = SetUpFakeService(ServiceData(
      ServiceKind::PRIMARY, kStartHandle, kEndHandle, kTestServiceUuid1));
  constexpr UUID kCharUuid(uint16_t{0xfefe});

  size_t read_count = 0;
  fake_client()->set_read_by_type_request_callback(
      [&](const UUID&, att::Handle start, att::Handle, auto callback) {
        ASSERT_EQ(0u, read_count++);
        EXPECT_EQ(kStartHandle, start);
        callback(fit::error(Client::ReadByTypeError{
            att::Error(att::ErrorCode::kReadNotPermitted), kEndHandle}));
      });

  std::optional<att::Result<>> status;
  std::vector<RemoteService::ReadByTypeResult> results;
  service->ReadByType(kCharUuid, [&](att::Result<> cb_status, auto cb_results) {
    status = cb_status;
    results = std::move(cb_results);
  });
  RunUntilIdle();
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(fit::ok(), *status);
  ASSERT_EQ(1u, results.size());
  EXPECT_EQ(CharacteristicHandle(kEndHandle), results[0].handle);
  EXPECT_EQ(att::ErrorCode::kReadNotPermitted, results[0].result.error_value());
}

TEST_F(RemoteServiceManagerTest,
       ReadByTypeResultOnLastHandleDoesNotOverflowHandle) {
  constexpr att::Handle kStartHandle = 0xFFFE;
  constexpr att::Handle kEndHandle = 0xFFFF;
  auto service = SetUpFakeService(ServiceData(
      ServiceKind::PRIMARY, kStartHandle, kEndHandle, kTestServiceUuid1));

  constexpr UUID kCharUuid(uint16_t{0xfefe});

  constexpr att::Handle kHandle = kEndHandle;
  const auto kValue = StaticByteBuffer(0x00, 0x01, 0x02);
  const std::vector<Client::ReadByTypeValue> kValues = {
      {kHandle, kValue.view(), /*maybe_truncated=*/false}};

  size_t read_count = 0;
  fake_client()->set_read_by_type_request_callback(
      [&](const UUID&, att::Handle start, att::Handle, auto callback) {
        ASSERT_EQ(0u, read_count++);
        EXPECT_EQ(kStartHandle, start);
        callback(fit::ok(kValues));
      });

  std::optional<att::Result<>> status;
  service->ReadByType(kCharUuid, [&](att::Result<> cb_status, auto values) {
    status = cb_status;
    ASSERT_EQ(1u, values.size());
    EXPECT_EQ(CharacteristicHandle(kHandle), values[0].handle);
    ASSERT_EQ(fit::ok(), values[0].result);
    EXPECT_TRUE(ContainersEqual(kValue, *values[0].result.value()));
  });

  RunUntilIdle();
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(fit::ok(), *status);
}

class RemoteServiceManagerServiceChangedTest : public RemoteServiceManagerTest {
 public:
  RemoteServiceManagerServiceChangedTest() = default;
  ~RemoteServiceManagerServiceChangedTest() override = default;

 protected:
  struct ServiceWatcherData {
    std::vector<att::Handle> removed;
    ServiceList added;
    ServiceList modified;
  };

  void SetUp() override {
    RemoteServiceManagerTest::SetUp();
    fake_client()->set_services({gatt_service()});
    fake_client()->set_characteristics({service_changed_characteristic()});
    fake_client()->set_descriptors({ccc_descriptor()});

    mgr()->set_service_watcher(
        [this](auto removed, ServiceList added, ServiceList modified) {
          svc_watcher_data_.push_back({removed, added, modified});
        });

    // Expect a Service Changed Client Characteristic Config descriptor write
    // that enables indications.
    fake_client()->set_write_request_callback(
        [this](att::Handle handle, const auto& value, auto status_callback) {
          write_request_count_++;
          EXPECT_EQ(ccc_descriptor_handle_, handle);
          EXPECT_TRUE(ContainersEqual(kCCCIndicateValue, value));
          status_callback(fit::ok());
        });

    att::Result<> status = ToResult(HostError::kFailed);
    mgr()->Initialize([&status](att::Result<> val) { status = val; },
                      NopMtuCallback);
    RunUntilIdle();
    EXPECT_EQ(fit::ok(), status);
    EXPECT_EQ(write_request_count_, 1);
    ASSERT_EQ(1u, svc_watcher_data_.size());
    ASSERT_EQ(1u, svc_watcher_data_[0].added.size());
    EXPECT_EQ(gatt_svc_start_handle_, svc_watcher_data_[0].added[0]->handle());
    EXPECT_EQ(types::kGenericAttributeService,
              svc_watcher_data_[0].added[0]->uuid());
    // Clear data so that tests start with index 0
    svc_watcher_data_.clear();
  }

  void TearDown() override { RemoteServiceManagerTest::TearDown(); }

  ServiceData gatt_service() const {
    return ServiceData(ServiceKind::PRIMARY,
                       gatt_svc_start_handle_,
                       gatt_svc_end_handle_,
                       types::kGenericAttributeService);
  }

  CharacteristicData service_changed_characteristic() const {
    return CharacteristicData(Property::kIndicate,
                              std::nullopt,
                              svc_changed_char_handle_,
                              svc_changed_char_value_handle_,
                              types::kServiceChangedCharacteristic);
  }

  DescriptorData ccc_descriptor() const {
    return DescriptorData(ccc_descriptor_handle_,
                          types::kClientCharacteristicConfig);
  }

  const std::vector<ServiceWatcherData>& svc_watcher_data() {
    return svc_watcher_data_;
  }

  int service_changed_ccc_write_count() const { return write_request_count_; }

 private:
  std::vector<ServiceWatcherData> svc_watcher_data_;
  int write_request_count_ = 0;
  const att::Handle gatt_svc_start_handle_ = 1;
  const att::Handle svc_changed_char_handle_ = 2;
  const att::Handle svc_changed_char_value_handle_ = 3;
  const att::Handle ccc_descriptor_handle_ = 4;
  const att::Handle gatt_svc_end_handle_ = 4;
};

// Tests that destroying RemoteServiceManager from within a RemoteService
// removal handler during Service Changed processing is handled safely without
// causing a use-after-free.
TEST_F(RemoteServiceManagerServiceChangedTest,
       RemovedHandlerDestroysManagerDoesntCauseUAF) {
  const att::Handle kService1StartHandle(5);
  const att::Handle kService1EndHandle(5);
  const att::Handle kService2StartHandle(6);
  const att::Handle kService2EndHandle(6);
  ServiceData service1(ServiceKind::PRIMARY,
                       kService1StartHandle,
                       kService1EndHandle,
                       kTestServiceUuid1);
  ServiceData service2(ServiceKind::PRIMARY,
                       kService2StartHandle,
                       kService2EndHandle,
                       kTestServiceUuid2);

  fake_client()->set_services({gatt_service(), service1, service2});
  auto service_changed_range_buffer =
      StaticByteBuffer(LowerBits(kService1StartHandle),
                       UpperBits(kService1StartHandle),
                       LowerBits(kService2EndHandle),
                       UpperBits(kService2EndHandle));
  fake_client()->SendNotification(
      /*indicate=*/true,
      service_changed_characteristic().value_handle,
      service_changed_range_buffer,
      /*maybe_truncated=*/false);
  RunUntilIdle();
  ASSERT_EQ(1u, svc_watcher_data().size());
  ASSERT_EQ(2u, svc_watcher_data()[0].added.size());

  bool destroyed = false;
  auto remove_handler = [this, &destroyed]() {
    if (!destroyed) {
      destroyed = true;
      DestroyServiceManager();
    }
  };
  svc_watcher_data()[0].added[0]->AddRemovedHandler(remove_handler);
  svc_watcher_data()[0].added[1]->AddRemovedHandler(remove_handler);

  // Clear the fixture's CCC-write expectation so that RemoteCharacteristic's
  // CCC-disable write during teardown does not produce a spurious failure.
  fake_client()->set_write_request_callback({});

  fake_client()->set_services({gatt_service()});
  fake_client()->SendNotification(
      /*indicate=*/true,
      service_changed_characteristic().value_handle,
      service_changed_range_buffer,
      /*maybe_truncated=*/false);
  RunUntilIdle();

  EXPECT_TRUE(destroyed);
}

TEST_F(RemoteServiceManagerServiceChangedTest,
       ServiceChangedNotificationWrongSizeBuffer) {
  const att::Handle kSvc1StartHandle(5);
  const att::Handle kSvc1EndHandle(kSvc1StartHandle);
  ServiceData svc1(ServiceKind::PRIMARY,
                   kSvc1StartHandle,
                   kSvc1EndHandle,
                   kTestServiceUuid1);
  fake_client()->set_services({gatt_service(), svc1});

  // Send a too small notification.
  auto svc_changed_range_buffer_too_small = StaticByteBuffer(0x01);
  fake_client()->SendNotification(/*indicate=*/true,
                                  service_changed_characteristic().value_handle,
                                  svc_changed_range_buffer_too_small,
                                  /*maybe_truncated=*/false);
  RunUntilIdle();
  // The notification should have been safely ignored.
  ASSERT_EQ(0u, svc_watcher_data().size());

  // Send a too large notification.
  StaticByteBuffer<sizeof(ServiceChangedCharacteristicValue) + 1>
      svc_changed_range_buffer_too_large =
          StaticByteBuffer(0x01, 0x02, 0x03, 0x04, 0x05);
  fake_client()->SendNotification(/*indicate=*/true,
                                  service_changed_characteristic().value_handle,
                                  svc_changed_range_buffer_too_large,
                                  /*maybe_truncated=*/false);
  RunUntilIdle();
  // The notification should have been safely ignored.
  ASSERT_EQ(0u, svc_watcher_data().size());
}

TEST_F(RemoteServiceManagerServiceChangedTest,
       ServiceChangedNotificationRangeStartGreaterThanRangeEnd) {
  const att::Handle kSvc1StartHandle(6);
  const att::Handle kSvc1EndHandle(7);
  ServiceData svc1(ServiceKind::PRIMARY,
                   kSvc1StartHandle,
                   kSvc1EndHandle,
                   kTestServiceUuid1);
  fake_client()->set_services({gatt_service(), svc1});

  // Send notification with start/end handles swapped.
  auto svc_changed_range_buffer = StaticByteBuffer(
      LowerBits(kSvc1EndHandle),
      UpperBits(kSvc1EndHandle),  // start handle of affected range
      LowerBits(kSvc1StartHandle),
      UpperBits(kSvc1StartHandle)  // end handle of affected range
  );
  fake_client()->SendNotification(/*indicate=*/true,
                                  service_changed_characteristic().value_handle,
                                  svc_changed_range_buffer,
                                  /*maybe_truncated=*/false);
  RunUntilIdle();
  // The notification should have been safely ignored.
  ASSERT_EQ(0u, svc_watcher_data().size());
}

TEST_F(RemoteServiceManagerServiceChangedTest, AddModifyAndRemoveService) {
  // Add a test service to ensure that service discovery occurs after the
  // Service Changed characteristic is configured. The test service has a
  // characteristic that supports indications in order to test that
  // notifications aren't disabled when the service is modified or removed.
  const att::Handle kSvc1StartHandle(5);
  const att::Handle kSvc1ChrcHandle(6);
  const att::Handle kSvc1ChrcValueHandle(7);
  const att::Handle kSvc1CCCHandle(8);
  const UUID kSvc1ChrcUuid(kTestUuid3);
  const att::Handle kSvc1EndHandle(kSvc1CCCHandle);

  ServiceData svc1(ServiceKind::PRIMARY,
                   kSvc1StartHandle,
                   kSvc1EndHandle,
                   kTestServiceUuid1);
  CharacteristicData svc1_characteristic(Property::kIndicate,
                                         std::nullopt,
                                         kSvc1ChrcHandle,
                                         kSvc1ChrcValueHandle,
                                         kSvc1ChrcUuid);
  DescriptorData svc1_descriptor(kSvc1CCCHandle,
                                 types::kClientCharacteristicConfig);
  fake_client()->set_services({gatt_service(), svc1});
  fake_client()->set_characteristics(
      {service_changed_characteristic(), svc1_characteristic});
  fake_client()->set_descriptors({ccc_descriptor(), svc1_descriptor});

  // Send a notification that svc1 has been added.
  auto svc_changed_range_buffer = StaticByteBuffer(
      LowerBits(kSvc1StartHandle),
      UpperBits(kSvc1StartHandle),  // start handle of affected range
      LowerBits(kSvc1EndHandle),
      UpperBits(kSvc1EndHandle)  // end handle of affected range
  );
  fake_client()->SendNotification(/*indicate=*/true,
                                  service_changed_characteristic().value_handle,
                                  svc_changed_range_buffer,
                                  /*maybe_truncated=*/false);
  RunUntilIdle();
  ASSERT_EQ(1u, svc_watcher_data().size());
  ASSERT_EQ(1u, svc_watcher_data()[0].added.size());
  EXPECT_EQ(0u, svc_watcher_data()[0].removed.size());
  EXPECT_EQ(0u, svc_watcher_data()[0].modified.size());
  EXPECT_EQ(kSvc1StartHandle, svc_watcher_data()[0].added[0]->handle());
  bool original_service_removed = false;
  svc_watcher_data()[0].added[0]->AddRemovedHandler(
      [&]() { original_service_removed = true; });

  // Discover the characteristic with the CCC descriptor before enabling
  // characteristic value notifications.
  svc_watcher_data()[0].added[0]->DiscoverCharacteristics(
      [&](att::Result<> status, const CharacteristicMap& characteristics) {
        EXPECT_EQ(fit::ok(), status);
        EXPECT_EQ(characteristics.size(), 1u);
      });
  RunUntilIdle();

  // Expect writes to the service's CCC descriptor when notifications are
  // enabled.
  int svc1_ccc_write_request_count = 0;
  fake_client()->set_write_request_callback(
      [&](att::Handle handle, const auto& value, auto status_callback) {
        svc1_ccc_write_request_count++;
        EXPECT_EQ(kSvc1CCCHandle, handle);
        EXPECT_TRUE(ContainersEqual(kCCCIndicateValue, value));
        status_callback(fit::ok());
      });

  std::optional<att::Result<>> original_notification_status;
  svc_watcher_data()[0].added[0]->EnableNotifications(
      bt::gatt::CharacteristicHandle(kSvc1ChrcValueHandle),
      NopValueCallback,
      [&](att::Result<> cb_status, IdType) {
        original_notification_status = cb_status;
      });
  RunUntilIdle();
  ASSERT_TRUE(original_notification_status);
  EXPECT_EQ(fit::ok(), *original_notification_status);
  EXPECT_EQ(svc1_ccc_write_request_count, 1);

  // Send a notification that svc1 has been modified. Service Changed
  // notifications guarantee that all services within their range have been
  // modified if they are still present after a fresh service discovery, so we
  // can just send the same range again. (Core Spec v5.3, Vol 3, Part G,
  // Sec 7.1)
  fake_client()->SendNotification(/*indicate=*/true,
                                  service_changed_characteristic().value_handle,
                                  svc_changed_range_buffer,
                                  /*maybe_truncated=*/false);
  RunUntilIdle();
  EXPECT_TRUE(original_service_removed);
  // CCC should not be written to when the service is modified.
  EXPECT_EQ(svc1_ccc_write_request_count, 1);
  ASSERT_EQ(2u, svc_watcher_data().size());
  EXPECT_EQ(0u, svc_watcher_data()[1].added.size());
  EXPECT_EQ(0u, svc_watcher_data()[1].removed.size());
  ASSERT_EQ(1u, svc_watcher_data()[1].modified.size());
  EXPECT_EQ(kSvc1StartHandle, svc_watcher_data()[1].modified[0]->handle());
  bool modified_service_removed = false;
  svc_watcher_data()[1].modified[0]->AddRemovedHandler(
      [&]() { modified_service_removed = true; });

  svc_watcher_data()[1].modified[0]->DiscoverCharacteristics(
      [&](att::Result<> status, const CharacteristicMap& characteristics) {
        EXPECT_EQ(fit::ok(), status);
        EXPECT_EQ(characteristics.size(), 1u);
      });
  RunUntilIdle();

  std::optional<att::Result<>> modified_notification_status;
  svc_watcher_data()[1].modified[0]->EnableNotifications(
      bt::gatt::CharacteristicHandle(kSvc1ChrcValueHandle),
      NopValueCallback,
      [&](att::Result<> cb_status, IdType) {
        modified_notification_status = cb_status;
      });
  RunUntilIdle();
  ASSERT_TRUE(modified_notification_status);
  EXPECT_EQ(fit::ok(), *modified_notification_status);
  EXPECT_EQ(svc1_ccc_write_request_count, 2);

  // Remove svc1.
  fake_client()->set_services({gatt_service()});

  // Send a notification that svc1 has been removed.
  fake_client()->SendNotification(/*indicate=*/true,
                                  service_changed_characteristic().value_handle,
                                  svc_changed_range_buffer,
                                  /*maybe_truncated=*/false);
  RunUntilIdle();
  EXPECT_TRUE(modified_service_removed);
  // CCC should not be written to when the service is removed.
  EXPECT_EQ(svc1_ccc_write_request_count, 2);
  ASSERT_EQ(3u, svc_watcher_data().size());
  EXPECT_EQ(0u, svc_watcher_data()[2].added.size());
  ASSERT_EQ(1u, svc_watcher_data()[2].removed.size());
  EXPECT_EQ(0u, svc_watcher_data()[2].modified.size());
  EXPECT_EQ(kSvc1StartHandle, svc_watcher_data()[2].removed[0]);
  EXPECT_EQ(svc1_ccc_write_request_count, 2);
}

// A Service Changed notification received during initialization (service
// discovery) should be queued and processed after service discovery completes
// (as the last step of initialization). The service watcher should only be
// called once.
TEST_F(RemoteServiceManagerTest, ServiceChangedDuringInitialization) {
  const att::Handle kGattSvcStartHandle(1);
  const att::Handle kSvcChangedChrcHandle(2);
  const att::Handle kSvcChangedChrcValueHandle(3);
  const att::Handle kCCCDescriptorHandle(4);
  const att::Handle kGattSvcEndHandle(kCCCDescriptorHandle);
  const att::Handle kSvc1StartHandle(5);
  const att::Handle kSvc1EndHandle(kSvc1StartHandle);

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
  ServiceData svc1(ServiceKind::PRIMARY,
                   kSvc1StartHandle,
                   kSvc1EndHandle,
                   kTestServiceUuid1);
  fake_client()->set_services({gatt_svc, svc1});
  fake_client()->set_characteristics({service_changed_chrc});
  fake_client()->set_descriptors({ccc_descriptor});

  int svc_watcher_count = 0;
  mgr()->set_service_watcher([&](std::vector<att::Handle> removed,
                                 ServiceList added,
                                 ServiceList modified) {
    EXPECT_EQ(0u, removed.size());
    EXPECT_EQ(2u, added.size());
    EXPECT_EQ(0u, modified.size());
    svc_watcher_count++;
  });

  // Send a notification during primary service discovery (i.e. the second
  // discovery, as the first is for discovering the GATT Service).
  int discover_services_count = 0;
  fake_client()->set_discover_services_callback([&](ServiceKind /*kind*/) {
    if (discover_services_count == 1) {
      auto svc_changed_range_buffer = StaticByteBuffer(
          LowerBits(kSvc1StartHandle),
          UpperBits(kSvc1StartHandle),  // start handle of affected range
          LowerBits(kSvc1EndHandle),
          UpperBits(kSvc1EndHandle)  // end handle of affected range
      );
      fake_client()->SendNotification(/*indicate=*/true,
                                      kSvcChangedChrcValueHandle,
                                      svc_changed_range_buffer,
                                      /*maybe_truncated=*/false);
    }
    discover_services_count++;
    return fit::ok();
  });

  // Expect a Service Changed Client Characteristic Config descriptor write that
  // enables indications.
  int write_request_count = 0;
  fake_client()->set_write_request_callback(
      [&](att::Handle handle, const auto& value, auto status_callback) {
        write_request_count++;
        EXPECT_EQ(kCCCDescriptorHandle, handle);
        EXPECT_TRUE(ContainersEqual(kCCCIndicateValue, value));
        status_callback(fit::ok());
      });

  att::Result<> status = ToResult(HostError::kFailed);
  mgr()->Initialize(
      [&](att::Result<> val) {
        status = val;
        EXPECT_EQ(1, svc_watcher_count);
      },
      NopMtuCallback);
  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(1, write_request_count);
  EXPECT_EQ(1, svc_watcher_count);
}

TEST_F(RemoteServiceManagerServiceChangedTest,
       SecondServiceChangedNotificationIsQueued) {
  const att::Handle kSvc1StartHandle(5);
  const att::Handle kSvc1EndHandle(kSvc1StartHandle);

  // Add a test service to ensure that service discovery occurs after the
  // Service Changed characteristic is configured.
  ServiceData svc1(ServiceKind::PRIMARY,
                   kSvc1StartHandle,
                   kSvc1EndHandle,
                   kTestServiceUuid1);
  fake_client()->set_services({gatt_service(), svc1});

  // Send a notification that svc1 has been added.
  auto svc_changed_range_buffer = StaticByteBuffer(
      LowerBits(kSvc1StartHandle),
      UpperBits(kSvc1StartHandle),  // start handle of affected range
      LowerBits(kSvc1EndHandle),
      UpperBits(kSvc1EndHandle)  // end handle of affected range
  );
  fake_client()->SendNotification(/*indicate=*/true,
                                  service_changed_characteristic().value_handle,
                                  svc_changed_range_buffer,
                                  /*maybe_truncated=*/false);
  // Send a notification that svc1 has been modified.
  fake_client()->SendNotification(/*indicate=*/true,
                                  service_changed_characteristic().value_handle,
                                  svc_changed_range_buffer,
                                  /*maybe_truncated=*/false);

  RunUntilIdle();
  ASSERT_EQ(2u, svc_watcher_data().size());

  ASSERT_EQ(1u, svc_watcher_data()[0].added.size());
  EXPECT_EQ(0u, svc_watcher_data()[0].removed.size());
  EXPECT_EQ(0u, svc_watcher_data()[0].modified.size());
  EXPECT_FALSE(svc_watcher_data()[0]
                   .added[0]
                   .is_alive());  // WeakPtr should already be invalidated

  EXPECT_EQ(0u, svc_watcher_data()[1].added.size());
  EXPECT_EQ(0u, svc_watcher_data()[1].removed.size());
  ASSERT_EQ(1u, svc_watcher_data()[1].modified.size());
  EXPECT_EQ(kSvc1StartHandle, svc_watcher_data()[1].modified[0]->handle());
}

TEST_F(RemoteServiceManagerServiceChangedTest, ServiceUuidChanged) {
  const att::Handle kSvcStartHandle(5);
  const att::Handle kSvcEndHandle(kSvcStartHandle);

  ServiceData svc1(
      ServiceKind::PRIMARY, kSvcStartHandle, kSvcEndHandle, kTestServiceUuid1);
  fake_client()->set_services({gatt_service(), svc1});

  // Send a notification that svc1 has been added.
  auto svc_changed_range_buffer = StaticByteBuffer(
      LowerBits(kSvcStartHandle),
      UpperBits(kSvcStartHandle),  // start handle of affected range
      LowerBits(kSvcEndHandle),
      UpperBits(kSvcEndHandle)  // end handle of affected range
  );
  fake_client()->SendNotification(/*indicate=*/true,
                                  service_changed_characteristic().value_handle,
                                  svc_changed_range_buffer,
                                  /*maybe_truncated=*/false);

  RunUntilIdle();
  ASSERT_EQ(1u, svc_watcher_data().size());

  ASSERT_EQ(1u, svc_watcher_data()[0].added.size());
  EXPECT_EQ(0u, svc_watcher_data()[0].removed.size());
  EXPECT_EQ(0u, svc_watcher_data()[0].modified.size());
  EXPECT_EQ(kSvcStartHandle, svc_watcher_data()[0].added[0]->handle());
  EXPECT_EQ(kTestServiceUuid1, svc_watcher_data()[0].added[0]->uuid());

  ServiceData svc2(
      ServiceKind::PRIMARY, kSvcStartHandle, kSvcEndHandle, kTestServiceUuid2);
  fake_client()->set_services({gatt_service(), svc2});

  // Send a notification that svc2 has replaced svc1.
  fake_client()->SendNotification(/*indicate=*/true,
                                  service_changed_characteristic().value_handle,
                                  svc_changed_range_buffer,
                                  /*maybe_truncated=*/false);
  RunUntilIdle();
  ASSERT_EQ(2u, svc_watcher_data().size());

  ASSERT_EQ(1u, svc_watcher_data()[1].added.size());
  ASSERT_EQ(1u, svc_watcher_data()[1].removed.size());
  EXPECT_EQ(0u, svc_watcher_data()[1].modified.size());
  EXPECT_EQ(kSvcStartHandle, svc_watcher_data()[1].removed[0]);
  EXPECT_EQ(kSvcStartHandle, svc_watcher_data()[1].added[0]->handle());
  EXPECT_EQ(kTestServiceUuid2, svc_watcher_data()[1].added[0]->uuid());
}

TEST_F(
    RemoteServiceManagerServiceChangedTest,
    AddServiceThenRemoveServiceAndAddTwoMoreServicesBeforeAndAfterRemovedServiceWithSameNotification) {
  const att::Handle kSvc1StartHandle(7);
  const att::Handle kSvc1EndHandle(kSvc1StartHandle);
  const att::Handle kSvc2StartHandle(6);
  const att::Handle kSvc2EndHandle(kSvc2StartHandle);
  const att::Handle kSvc3StartHandle(8);
  const att::Handle kSvc3EndHandle(kSvc3StartHandle);

  ServiceData svc1(ServiceKind::PRIMARY,
                   kSvc1StartHandle,
                   kSvc1EndHandle,
                   kTestServiceUuid1);
  fake_client()->set_services({gatt_service(), svc1});

  // Send a notification that svc1 has been added.
  auto svc_changed_range_buffer_0 = StaticByteBuffer(
      LowerBits(kSvc1StartHandle),
      UpperBits(kSvc1StartHandle),  // start handle of affected range
      LowerBits(kSvc1EndHandle),
      UpperBits(kSvc1EndHandle)  // end handle of affected range
  );
  fake_client()->SendNotification(/*indicate=*/true,
                                  service_changed_characteristic().value_handle,
                                  svc_changed_range_buffer_0,
                                  /*maybe_truncated=*/false);

  RunUntilIdle();
  ASSERT_EQ(1u, svc_watcher_data().size());
  ASSERT_EQ(1u, svc_watcher_data()[0].added.size());
  EXPECT_EQ(0u, svc_watcher_data()[0].removed.size());
  EXPECT_EQ(0u, svc_watcher_data()[0].modified.size());
  EXPECT_EQ(kSvc1StartHandle, svc_watcher_data()[0].added[0]->handle());

  ServiceData svc2(ServiceKind::PRIMARY,
                   kSvc2StartHandle,
                   kSvc2EndHandle,
                   kTestServiceUuid2);
  ServiceData svc3(ServiceKind::PRIMARY,
                   kSvc3StartHandle,
                   kSvc3EndHandle,
                   kTestServiceUuid3);
  fake_client()->set_services({gatt_service(), svc2, svc3});

  // Range includes all 3 services.
  auto svc_changed_range_buffer_1 = StaticByteBuffer(
      LowerBits(kSvc2StartHandle),
      UpperBits(kSvc2StartHandle),  // start handle of affected range
      LowerBits(kSvc3EndHandle),
      UpperBits(kSvc3EndHandle)  // end handle of affected range
  );
  fake_client()->SendNotification(/*indicate=*/true,
                                  service_changed_characteristic().value_handle,
                                  svc_changed_range_buffer_1,
                                  /*maybe_truncated=*/false);

  RunUntilIdle();
  ASSERT_EQ(2u, svc_watcher_data().size());
  ASSERT_EQ(2u, svc_watcher_data()[1].added.size());
  ASSERT_EQ(1u, svc_watcher_data()[1].removed.size());
  EXPECT_EQ(0u, svc_watcher_data()[1].modified.size());
  EXPECT_EQ(kSvc1StartHandle, svc_watcher_data()[1].removed[0]);
  EXPECT_EQ(kSvc2StartHandle, svc_watcher_data()[1].added[0]->handle());
  EXPECT_EQ(kSvc3StartHandle, svc_watcher_data()[1].added[1]->handle());
}

class RemoteServiceManagerServiceChangedTestWithServiceKindParam
    : public RemoteServiceManagerServiceChangedTest,
      public ::testing::WithParamInterface<ServiceKind> {};

TEST_P(RemoteServiceManagerServiceChangedTestWithServiceKindParam,
       ServiceChangedServiceDiscoveryFailureThenSuccess) {
  const ServiceKind kKind = GetParam();

  const att::Handle kSvc1StartHandle(5);
  const att::Handle kSvc1EndHandle(kSvc1StartHandle);
  ServiceData svc1(kKind, kSvc1StartHandle, kSvc1EndHandle, kTestServiceUuid1);
  const att::Handle kSvc2StartHandle(7);
  const att::Handle kSvc2EndHandle(kSvc2StartHandle);
  ServiceData svc2(kKind, kSvc2StartHandle, kSvc2EndHandle, kTestServiceUuid2);
  fake_client()->set_services({gatt_service(), svc1, svc2});

  // Cause only the first service discovery to fail.
  int discover_services_count = 0;
  fake_client()->set_discover_services_callback(
      [&](ServiceKind kind) -> att::Result<> {
        if (kind == kKind) {
          discover_services_count++;
          if (discover_services_count == 1) {
            return ToResult(HostError::kFailed);
          }
        }
        return fit::ok();
      });

  auto svc1_changed_range_buffer = StaticByteBuffer(
      LowerBits(kSvc1StartHandle),
      UpperBits(kSvc1StartHandle),  // start handle of affected range
      LowerBits(kSvc1EndHandle),
      UpperBits(kSvc1EndHandle)  // end handle of affected range
  );
  fake_client()->SendNotification(/*indicate=*/true,
                                  service_changed_characteristic().value_handle,
                                  svc1_changed_range_buffer,
                                  /*maybe_truncated=*/false);

  // This second notification should be queued and processed after the first
  // service discovery fails.
  auto svc2_changed_range_buffer = StaticByteBuffer(
      LowerBits(kSvc2StartHandle),
      UpperBits(kSvc2StartHandle),  // start handle of affected range
      LowerBits(kSvc2EndHandle),
      UpperBits(kSvc2EndHandle)  // end handle of affected range
  );
  fake_client()->SendNotification(/*indicate=*/true,
                                  service_changed_characteristic().value_handle,
                                  svc2_changed_range_buffer,
                                  /*maybe_truncated=*/false);

  RunUntilIdle();
  ASSERT_EQ(1u, svc_watcher_data().size());
  ASSERT_EQ(1u, svc_watcher_data()[0].added.size());
  EXPECT_EQ(0u, svc_watcher_data()[0].removed.size());
  EXPECT_EQ(0u, svc_watcher_data()[0].modified.size());
  EXPECT_EQ(kSvc2StartHandle, svc_watcher_data()[0].added[0]->handle());
}

INSTANTIATE_TEST_SUITE_P(
    ServiceKind,
    RemoteServiceManagerServiceChangedTestWithServiceKindParam,
    ::testing::Values(ServiceKind::PRIMARY, ServiceKind::SECONDARY));

TEST_F(RemoteServiceManagerServiceChangedTest,
       SecondaryServiceDiscoveryIgnoresUnsupportedGroupTypeError) {
  const att::Handle kSvc1StartHandle(5);
  const att::Handle kSvc1EndHandle(kSvc1StartHandle);

  ServiceData svc1(ServiceKind::PRIMARY,
                   kSvc1StartHandle,
                   kSvc1EndHandle,
                   kTestServiceUuid1);
  fake_client()->set_services({gatt_service(), svc1});

  fake_client()->set_discover_services_callback([](ServiceKind kind) {
    if (kind == ServiceKind::SECONDARY) {
      return ToResult(att::ErrorCode::kUnsupportedGroupType);
    }
    return att::Result<>(fit::ok());
  });

  // Send a notification that svc1 has been added.
  auto svc_changed_range_buffer = StaticByteBuffer(
      LowerBits(kSvc1StartHandle),
      UpperBits(kSvc1StartHandle),  // start handle of affected range
      LowerBits(kSvc1EndHandle),
      UpperBits(kSvc1EndHandle)  // end handle of affected range
  );
  fake_client()->SendNotification(/*indicate=*/true,
                                  service_changed_characteristic().value_handle,
                                  svc_changed_range_buffer,
                                  /*maybe_truncated=*/false);
  RunUntilIdle();
  ASSERT_EQ(1u, svc_watcher_data().size());
  ASSERT_EQ(1u, svc_watcher_data()[0].added.size());
  EXPECT_EQ(0u, svc_watcher_data()[0].removed.size());
  EXPECT_EQ(0u, svc_watcher_data()[0].modified.size());
  EXPECT_EQ(kSvc1StartHandle, svc_watcher_data()[0].added[0]->handle());
}

TEST_F(RemoteServiceManagerServiceChangedTest, GattProfileServiceChanged) {
  EXPECT_EQ(1, service_changed_ccc_write_count());

  StaticByteBuffer svc_changed_range_buffer(
      LowerBits(gatt_service().range_start),
      UpperBits(gatt_service().range_start),  // start handle of affected range
      LowerBits(gatt_service().range_end),
      UpperBits(gatt_service().range_end)  // end handle of affected range
  );
  fake_client()->SendNotification(/*indicate=*/true,
                                  service_changed_characteristic().value_handle,
                                  svc_changed_range_buffer,
                                  /*maybe_truncated=*/false);
  RunUntilIdle();
  ASSERT_EQ(1u, svc_watcher_data().size());
  EXPECT_EQ(0u, svc_watcher_data()[0].added.size());
  EXPECT_EQ(0u, svc_watcher_data()[0].removed.size());
  ASSERT_EQ(1u, svc_watcher_data()[0].modified.size());
  EXPECT_EQ(gatt_service().range_start,
            svc_watcher_data()[0].modified[0]->handle());
  EXPECT_EQ(1, service_changed_ccc_write_count());

  // The handler for notifications should remain configured.
  fake_client()->SendNotification(/*indicate=*/true,
                                  service_changed_characteristic().value_handle,
                                  svc_changed_range_buffer,
                                  /*maybe_truncated=*/false);
  RunUntilIdle();
  ASSERT_EQ(2u, svc_watcher_data().size());
  EXPECT_EQ(0u, svc_watcher_data()[1].added.size());
  EXPECT_EQ(0u, svc_watcher_data()[1].removed.size());
  ASSERT_EQ(1u, svc_watcher_data()[1].modified.size());
  EXPECT_EQ(gatt_service().range_start,
            svc_watcher_data()[1].modified[0]->handle());
  EXPECT_EQ(1, service_changed_ccc_write_count());
  // A new service should not have been created for the modified GATT Profile
  // service.
  EXPECT_EQ(&svc_watcher_data()[0].modified[0].get(),
            &svc_watcher_data()[1].modified[0].get());
}

TEST_F(RemoteServiceManagerTest, ErrorDiscoveringGattProfileService) {
  ServiceData gatt_svc(
      ServiceKind::PRIMARY, 1, 1, types::kGenericAttributeService);
  ServiceData svc1(ServiceKind::PRIMARY, 2, 2, kTestServiceUuid1);
  std::vector<ServiceData> fake_services{{gatt_svc, svc1}};
  fake_client()->set_services(std::move(fake_services));

  fake_client()->set_discover_services_callback([](ServiceKind kind) {
    if (kind == ServiceKind::PRIMARY) {
      return ToResult(att::ErrorCode::kRequestNotSupported);
    }
    return att::Result<>(fit::ok());
  });

  std::optional<att::Result<>> status;
  mgr()->Initialize([&status](att::Result<> val) { status = val; },
                    NopMtuCallback);
  RunUntilIdle();
  EXPECT_EQ(ToResult(att::ErrorCode::kRequestNotSupported), *status);
}

TEST_F(RemoteServiceManagerTest,
       MultipleGattProfileServicesFailsInitialization) {
  ServiceData gatt_svc0(
      ServiceKind::PRIMARY, 1, 1, types::kGenericAttributeService);
  ServiceData gatt_svc1(
      ServiceKind::PRIMARY, 2, 2, types::kGenericAttributeService);
  std::vector<ServiceData> fake_services{{gatt_svc0, gatt_svc1}};
  fake_client()->set_services(std::move(fake_services));

  std::optional<att::Result<>> status;
  mgr()->Initialize([&status](att::Result<> val) { status = val; },
                    NopMtuCallback);
  RunUntilIdle();
  EXPECT_EQ(ToResult(HostError::kFailed), *status);
}

TEST_F(RemoteServiceManagerTest, InitializeEmptyGattProfileService) {
  ServiceData gatt_svc(
      ServiceKind::PRIMARY, 1, 1, types::kGenericAttributeService);
  ServiceData svc1(ServiceKind::PRIMARY, 2, 2, kTestServiceUuid1);
  std::vector<ServiceData> fake_services{{gatt_svc, svc1}};
  fake_client()->set_services(std::move(fake_services));

  ServiceList services;
  mgr()->set_service_watcher(
      [&services](auto /*removed*/, ServiceList added, auto /*modified*/) {
        services.insert(services.end(), added.begin(), added.end());
      });

  att::Result<> status = ToResult(HostError::kFailed);
  mgr()->Initialize([&status](att::Result<> val) { status = val; },
                    NopMtuCallback);
  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  ASSERT_EQ(2u, services.size());
  EXPECT_EQ(gatt_svc.range_start, services[0]->handle());
  EXPECT_EQ(gatt_svc.type, services[0]->uuid());
  EXPECT_EQ(svc1.range_start, services[1]->handle());
  EXPECT_EQ(svc1.type, services[1]->uuid());
}

TEST_F(RemoteServiceManagerTest,
       EnableServiceChangedNotificationsReturnsError) {
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

  ServiceList services;
  mgr()->set_service_watcher(
      [&services](auto /*removed*/, ServiceList added, auto /*modified*/) {
        services.insert(services.end(), added.begin(), added.end());
      });

  // Return an error when a Service Changed Client Characteristic Config
  // descriptor write is performed.
  int write_request_count = 0;
  fake_client()->set_write_request_callback(
      [&](att::Handle handle, const auto&, auto status_callback) {
        write_request_count++;
        EXPECT_EQ(kCCCDescriptorHandle, handle);
        status_callback(ToResult(att::ErrorCode::kWriteNotPermitted));
      });

  std::optional<att::Result<>> status;
  mgr()->Initialize([&status](att::Result<> val) { status = val; },
                    NopMtuCallback);
  RunUntilIdle();
  EXPECT_EQ(ToResult(att::ErrorCode::kWriteNotPermitted), *status);
  EXPECT_EQ(write_request_count, 1);
}

TEST_F(RemoteServiceManagerTest,
       ErrorDiscoveringGattProfileServiceCharacteristics) {
  ServiceData gatt_svc(
      ServiceKind::PRIMARY, 1, 3, types::kGenericAttributeService);
  ServiceData svc1(ServiceKind::PRIMARY, 4, 4, kTestServiceUuid1);
  std::vector<ServiceData> fake_services{{gatt_svc, svc1}};
  fake_client()->set_services(std::move(fake_services));

  fake_client()->set_characteristic_discovery_status(
      ToResult(att::ErrorCode::kRequestNotSupported));

  std::optional<att::Result<>> status;
  mgr()->Initialize([&status](att::Result<> val) { status = val; },
                    NopMtuCallback);
  RunUntilIdle();
  EXPECT_EQ(ToResult(att::ErrorCode::kRequestNotSupported), *status);
}

TEST_F(RemoteServiceManagerTest, DisableNotificationInHandlerCallback) {
  const CharacteristicHandle kChrcValueHandle(3);
  RemoteService::WeakPtr svc = SetupNotifiableService();
  std::optional<IdType> handler_id;
  RemoteCharacteristic::NotifyStatusCallback status_cb =
      [&](att::Result<> status, IdType cb_handler_id) {
        EXPECT_EQ(fit::ok(), status);
        handler_id = cb_handler_id;
      };

  int value_cb_count = 0;
  RemoteCharacteristic::ValueCallback value_cb = [&](auto&, auto) {
    value_cb_count++;
    ASSERT_TRUE(handler_id);
    // Disabling notifications in handler should not crash.
    svc->DisableNotifications(
        kChrcValueHandle, handler_id.value(), [](auto) {});
  };
  svc->EnableNotifications(
      kChrcValueHandle, std::move(value_cb), std::move(status_cb));

  fake_client()->SendNotification(/*indicate=*/false,
                                  kChrcValueHandle.value,
                                  StaticByteBuffer('y', 'e'),
                                  /*maybe_truncated=*/false);
  RunUntilIdle();
  EXPECT_EQ(value_cb_count, 1);

  // Second notification should not notify disabled handler.
  fake_client()->SendNotification(/*indicate=*/false,
                                  kChrcValueHandle.value,
                                  StaticByteBuffer('y', 'e'),
                                  /*maybe_truncated=*/false);
  RunUntilIdle();
  EXPECT_EQ(value_cb_count, 1);
}

TEST_F(RemoteServiceManagerServiceChangedTest,
       ServiceRemovedDuringReadLongCharacteristic) {
  const att::Handle kSvc1StartHandle(5);
  const att::Handle kSvc1ChrcHandle(6);
  const att::Handle kSvc1ChrcValueHandle(7);
  const att::Handle kSvc1EndHandle(kSvc1ChrcValueHandle);
  ServiceData svc1(ServiceKind::PRIMARY,
                   kSvc1StartHandle,
                   kSvc1EndHandle,
                   kTestServiceUuid1);
  const UUID kSvc1ChrcUuid(kTestUuid3);
  CharacteristicData svc1_characteristic(Property::kRead,
                                         std::nullopt,
                                         kSvc1ChrcHandle,
                                         kSvc1ChrcValueHandle,
                                         kSvc1ChrcUuid);
  fake_client()->set_services({gatt_service(), svc1});
  fake_client()->set_characteristics(
      {service_changed_characteristic(), svc1_characteristic});
  fake_client()->set_descriptors({ccc_descriptor()});

  // Send a notification that svc1 has been added.
  auto svc_changed_range_buffer = StaticByteBuffer(
      LowerBits(kSvc1StartHandle),
      UpperBits(kSvc1StartHandle),  // start handle of affected range
      LowerBits(kSvc1EndHandle),
      UpperBits(kSvc1EndHandle)  // end handle of affected range
  );
  fake_client()->SendNotification(/*indicate=*/true,
                                  service_changed_characteristic().value_handle,
                                  svc_changed_range_buffer,
                                  /*maybe_truncated=*/false);
  RunUntilIdle();
  ASSERT_EQ(1u, svc_watcher_data().size());
  ASSERT_EQ(1u, svc_watcher_data()[0].added.size());
  EXPECT_EQ(kSvc1StartHandle, svc_watcher_data()[0].added[0]->handle());

  RemoteService::WeakPtr service = svc_watcher_data()[0].added[0];
  service->DiscoverCharacteristics([](auto, const auto&) {});
  RunUntilIdle();

  int read_req_count = 0;
  RemoteService::ReadValueCallback read_callback = nullptr;
  fake_client()->set_read_request_callback(
      [&](auto, RemoteService::ReadValueCallback callback) {
        EXPECT_EQ(read_req_count, 0);
        read_req_count++;
        read_callback = std::move(callback);
      });
  fake_client()->set_read_blob_request_callback(
      [](auto, auto, auto) { FAIL(); });

  int read_long_cb_count = 0;
  service->ReadLongCharacteristic(
      CharacteristicHandle(kSvc1ChrcValueHandle),
      /*offset=*/0,
      att::kMaxAttributeValueLength,
      [&](auto, auto&, auto) { read_long_cb_count++; });

  // Remove svc1.
  fake_client()->set_services({gatt_service()});
  fake_client()->SendNotification(/*indicate=*/true,
                                  service_changed_characteristic().value_handle,
                                  svc_changed_range_buffer,
                                  /*maybe_truncated=*/false);
  RunUntilIdle();
  EXPECT_FALSE(service.is_alive());
  ASSERT_EQ(2u, svc_watcher_data().size());
  ASSERT_EQ(1u, svc_watcher_data()[1].removed.size());
  EXPECT_EQ(read_req_count, 1);
  ASSERT_TRUE(read_callback);

  // Now that the service has been destroyed, the read long procedure should be
  // aborted when the first read request callback is called.
  StaticByteBuffer<att::kLEMinMTU - 1> expected_value;
  expected_value.Fill(0x02);
  read_callback(fit::ok(), expected_value.view(), /*maybe_truncated=*/true);
  RunUntilIdle();
  EXPECT_EQ(read_long_cb_count, 0);
}

}  // namespace
}  // namespace bt::gatt::internal
