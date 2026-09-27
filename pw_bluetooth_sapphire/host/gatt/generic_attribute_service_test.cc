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

#include "pw_bluetooth_sapphire/internal/host/gatt/generic_attribute_service.h"

#include <pw_assert/check.h>
#include <pw_bytes/endian.h>

#include "pw_bluetooth_sapphire/internal/host/gatt/gatt_defs.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/persisted_data.h"
#include "pw_unit_test/framework.h"

namespace bt::gatt {
namespace {
// Handles for the third attribute (Service Changed characteristic) and fourth
// attribute (corresponding client config).
constexpr att::Handle kChrcHandle = 0x0003;
constexpr att::Handle kCCCHandle = 0x0004;
// Handle for the ServerSupportedFeatures Chrc
constexpr att::Handle kSupportedFeaturesChrcHandle = 0x0005;
constexpr PeerId kTestPeerId(1);
constexpr uint16_t kEnableInd = 0x0002;

class GenericAttributeServiceTest : public ::testing::Test {
 protected:
  bool WriteServiceChangedCcc(PeerId peer_id,
                              uint16_t ccc_value,
                              fit::result<att::ErrorCode>* out_status) {
    PW_CHECK(out_status);

    auto* attr = mgr.database()->FindAttribute(kCCCHandle);
    PW_CHECK(attr);
    auto result_cb = [&out_status](auto cb_status) { *out_status = cb_status; };
    uint16_t value =
        pw::bytes::ConvertOrderTo(cpp20::endian::little, ccc_value);
    return attr->WriteAsync(
        peer_id, 0u, BufferView(&value, sizeof(value)), result_cb);
  }

  LocalServiceManager mgr;
};

// Test registration and unregistration of the local GATT service itself.
TEST_F(GenericAttributeServiceTest, RegisterUnregister) {
  {
    GenericAttributeService gatt_service(mgr.GetWeakPtr(), NopSendIndication);

    // Check that the local attribute database has a grouping for the GATT GATT
    // service with four attributes.
    auto iter = mgr.database()->groupings().begin();
    EXPECT_TRUE(iter->complete());
    EXPECT_EQ(6u, iter->attributes().size());
    EXPECT_TRUE(iter->active());
    EXPECT_EQ(0x0001, iter->start_handle());
    EXPECT_EQ(0x0006, iter->end_handle());
    EXPECT_EQ(types::kPrimaryService, iter->group_type());

    auto const* ccc_attr = mgr.database()->FindAttribute(kCCCHandle);
    ASSERT_TRUE(ccc_attr != nullptr);
    EXPECT_EQ(types::kClientCharacteristicConfig, ccc_attr->type());
  }

  // The service should now be unregistered, so no characeteristic attributes
  // should be active.
  auto const* chrc_attr = mgr.database()->FindAttribute(kChrcHandle);
  ASSERT_TRUE(chrc_attr == nullptr);
}

// Tests that registering the GATT service, enabling indication on its Service
// Changed characteristic, then registering a different service invokes the
// callback to send an indication to the "client."
TEST_F(GenericAttributeServiceTest, IndicateOnRegister) {
  std::optional<IdType> indicated_svc_id;
  auto send_indication =
      [&](IdType service_id, IdType chrc_id, PeerId peer_id, BufferView value) {
        EXPECT_EQ(kTestPeerId, peer_id);
        EXPECT_EQ(kServiceChangedChrcId, chrc_id);
        indicated_svc_id = service_id;

        ASSERT_EQ(4u, value.size());
        // The second service following the six-handle GATT service should
        // span the subsequent three handles.
        EXPECT_EQ(0x07, value[0]);
        EXPECT_EQ(0x00, value[1]);
        EXPECT_EQ(0x09, value[2]);
        EXPECT_EQ(0x00, value[3]);
      };

  // Register the GATT service.
  GenericAttributeService gatt_service(mgr.GetWeakPtr(),
                                       std::move(send_indication));

  // Enable Service Changed indications for the test client.
  fit::result<att::ErrorCode> status = fit::ok();
  WriteServiceChangedCcc(kTestPeerId, kEnableInd, &status);
  EXPECT_EQ(std::nullopt, indicated_svc_id);

  constexpr UUID kTestSvcType(uint32_t{0xdeadbeef});
  constexpr IdType kChrcId = 12;
  constexpr uint8_t kChrcProps = Property::kRead;
  constexpr UUID kTestChrcType(uint32_t{0xdeadbeef});
  const att::AccessRequirements kReadReqs(/*encryption=*/true,
                                          /*authentication=*/true,
                                          /*authorization=*/true);
  const att::AccessRequirements kWriteReqs, kUpdateReqs;
  auto service = std::make_unique<Service>(/*primary=*/false, kTestSvcType);
  service->AddCharacteristic(std::make_unique<Characteristic>(kChrcId,
                                                              kTestChrcType,
                                                              kChrcProps,
                                                              0,
                                                              kReadReqs,
                                                              kWriteReqs,
                                                              kUpdateReqs));
  auto service_id = mgr.RegisterService(
      std::move(service), NopReadHandler, NopWriteHandler, NopCCCallback);
  // Verify that service registration succeeded
  EXPECT_NE(kInvalidId, service_id);
  // Verify that |send_indication| was invoked to indicate the Service Changed
  // chrc within the gatt_service.
  EXPECT_EQ(gatt_service.service_id(), indicated_svc_id);
}

TEST_F(GenericAttributeServiceTest, ReadSupportedFeatures) {
  // Register the GATT service.
  GenericAttributeService gatt_service(mgr.GetWeakPtr(), NopSendIndication);
  auto const* ssf_attr =
      mgr.database()->FindAttribute(kSupportedFeaturesChrcHandle);
  ASSERT_TRUE(ssf_attr != nullptr);

  auto result_callback = [&](auto status, const ByteBuffer& value) {
    ASSERT_TRUE(status.is_ok());
    ASSERT_EQ(1u, value.size());
    ASSERT_EQ(0x00, value[0]);
  };

  ssf_attr->ReadAsync(PeerId(1), 0, result_callback);
}

// Same test as above, but the indication is enabled just prior unregistering
// the latter test service.
TEST_F(GenericAttributeServiceTest, IndicateOnUnregister) {
  std::optional<IdType> indicated_svc_id;
  auto send_indication =
      [&](IdType service_id, IdType chrc_id, PeerId peer_id, BufferView value) {
        EXPECT_EQ(kTestPeerId, peer_id);
        EXPECT_EQ(kServiceChangedChrcId, chrc_id);
        indicated_svc_id = service_id;

        ASSERT_EQ(4u, value.size());
        // The second service following the six-handle GATT service should
        // span the subsequent four handles (update enabled).
        EXPECT_EQ(0x07, value[0]);
        EXPECT_EQ(0x00, value[1]);
        EXPECT_EQ(0x0A, value[2]);
        EXPECT_EQ(0x00, value[3]);
      };

  // Register the GATT service.
  GenericAttributeService gatt_service(mgr.GetWeakPtr(),
                                       std::move(send_indication));

  constexpr UUID kTestSvcType(uint32_t{0xdeadbeef});
  constexpr IdType kChrcId = 0;
  constexpr uint8_t kChrcProps = Property::kNotify;
  constexpr UUID kTestChrcType(uint32_t{0xdeadbeef});
  const att::AccessRequirements kReadReqs, kWriteReqs;
  const att::AccessRequirements kUpdateReqs(/*encryption=*/true,
                                            /*authentication=*/true,
                                            /*authorization=*/true);
  auto service = std::make_unique<Service>(/*primary=*/false, kTestSvcType);
  service->AddCharacteristic(std::make_unique<Characteristic>(kChrcId,
                                                              kTestChrcType,
                                                              kChrcProps,
                                                              0,
                                                              kReadReqs,
                                                              kWriteReqs,
                                                              kUpdateReqs));
  auto service_id = mgr.RegisterService(
      std::move(service), NopReadHandler, NopWriteHandler, NopCCCallback);
  // Verify that service registration succeeded
  EXPECT_NE(kInvalidId, service_id);

  // Enable Service Changed indications for the test client.
  fit::result<att::ErrorCode> status = fit::ok();
  WriteServiceChangedCcc(kTestPeerId, kEnableInd, &status);
  EXPECT_EQ(std::nullopt, indicated_svc_id);

  mgr.UnregisterService(service_id);
  // Verify that |send_indication| was invoked to indicate the Service Changed
  // chrc within the gatt_service.
  EXPECT_EQ(gatt_service.service_id(), indicated_svc_id);
}

// Tests that registering the GATT service reads a persisted value for the
// service changed characteristic's ccc, and that enabling indication on its
// service changed characteristic writes a persisted value.
TEST_F(GenericAttributeServiceTest, PersistIndicate) {
  int persist_callback_count = 0;

  auto persist_callback = [&persist_callback_count](
                              PeerId peer_id,
                              ServiceChangedCCCPersistedData gatt_data) {
    EXPECT_EQ(peer_id, kTestPeerId);
    EXPECT_EQ(gatt_data.indicate, true);
    persist_callback_count++;
  };

  // Register the GATT service.
  GenericAttributeService gatt_service(mgr.GetWeakPtr(), NopSendIndication);
  gatt_service.SetPersistServiceChangedCCCCallback(std::move(persist_callback));
  EXPECT_EQ(persist_callback_count, 0);

  // Enable Service Changed indications for the test client.
  fit::result<att::ErrorCode> status = fit::ok();
  WriteServiceChangedCcc(kTestPeerId, kEnableInd, &status);
  EXPECT_EQ(persist_callback_count, 1);
}

// Tests that destroying a GenericAttributeService clears its registered
// callback in the LocalServiceManager to prevent a Use-After-Free crash when
// subsequent services are registered.
TEST_F(GenericAttributeServiceTest, UAFOnServiceChanged) {
  {
    GenericAttributeService gatt_service(mgr.GetWeakPtr(), NopSendIndication);

    // Enable Service Changed indications for the test client.
    fit::result<att::ErrorCode> status = fit::ok();
    WriteServiceChangedCcc(kTestPeerId, kEnableInd, &status);
    EXPECT_TRUE(status.is_ok());
  }

  constexpr UUID kTestSvcType(uint32_t{0xdeadbeef});
  auto service = std::make_unique<Service>(/*primary=*/false, kTestSvcType);
  mgr.RegisterService(
      std::move(service), NopReadHandler, NopWriteHandler, NopCCCallback);
}
}  // namespace
}  // namespace bt::gatt
