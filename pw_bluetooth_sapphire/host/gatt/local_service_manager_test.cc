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

#include "pw_bluetooth_sapphire/internal/host/gatt/local_service_manager.h"

#include <pw_assert/check.h>
#include <pw_bytes/endian.h>

#include <unordered_set>

#include "pw_bluetooth_sapphire/internal/host/att/att.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/gatt_defs.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/generic_attribute_service.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"
#include "pw_unit_test/framework.h"

namespace bt::gatt {
namespace {

constexpr PeerId kTestPeerId(1);
constexpr PeerId kTestPeerId2(2);
constexpr UUID kTestType16(uint16_t{0xdead});
constexpr UUID kTestType32(uint32_t{0xdeadbeef});

// The first characteristic value attribute of the first service has handle
// number 3.
constexpr att::Handle kFirstChrcValueHandle = 0x0003;

// The first descroptor of the first characteristic of the first service has
// handle number 4.
constexpr att::Handle kFirstDescrHandle = 0x0004;

inline att::AccessRequirements AllowedNoSecurity() {
  return att::AccessRequirements(/*encryption=*/false,
                                 /*authentication=*/false,
                                 /*authorization=*/false);
}

// Convenience function that registers |service| with |mgr| using the NOP
// handlers above by default.
IdType RegisterService(LocalServiceManager* mgr,
                       ServicePtr service,
                       ReadHandler read_handler = NopReadHandler,
                       WriteHandler write_handler = NopWriteHandler,
                       ClientConfigCallback ccc_callback = NopCCCallback) {
  return mgr->RegisterService(std::move(service),
                              std::move(read_handler),
                              std::move(write_handler),
                              std::move(ccc_callback));
}

TEST(LocalServiceManagerTest, EmptyService) {
  LocalServiceManager mgr;

  auto service = std::make_unique<Service>(/*primary=*/true, kTestType16);
  auto id1 = RegisterService(&mgr, std::move(service));
  EXPECT_NE(0u, id1);

  service = std::make_unique<Service>(/*primary=*/false, kTestType32);
  auto id2 = RegisterService(&mgr, std::move(service));
  EXPECT_NE(0u, id2);

  EXPECT_EQ(2u, mgr.database()->groupings().size());

  auto iter = mgr.database()->groupings().begin();

  EXPECT_TRUE(iter->complete());
  EXPECT_EQ(1u, iter->attributes().size());
  EXPECT_TRUE(iter->active());
  EXPECT_EQ(0x0001, iter->start_handle());
  EXPECT_EQ(0x0001, iter->end_handle());
  EXPECT_EQ(types::kPrimaryService, iter->group_type());
  EXPECT_TRUE(
      ContainersEqual(StaticByteBuffer(0xad, 0xde), iter->decl_value()));

  iter++;

  EXPECT_TRUE(iter->complete());
  EXPECT_EQ(1u, iter->attributes().size());
  EXPECT_TRUE(iter->active());
  EXPECT_EQ(0x0002, iter->start_handle());
  EXPECT_EQ(0x0002, iter->end_handle());
  EXPECT_EQ(types::kSecondaryService, iter->group_type());
  EXPECT_TRUE(ContainersEqual(StaticByteBuffer(0xFB,
                                               0x34,
                                               0x9B,
                                               0x5F,
                                               0x80,
                                               0x00,
                                               0x00,
                                               0x80,
                                               0x00,
                                               0x10,
                                               0x00,
                                               0x00,
                                               0xef,
                                               0xbe,
                                               0xad,
                                               0xde),
                              iter->decl_value()));
}

TEST(LocalServiceManagerTest, UnregisterService) {
  LocalServiceManager mgr;

  auto service = std::make_unique<Service>(/*primary=*/true, kTestType16);
  auto id1 = RegisterService(&mgr, std::move(service));
  EXPECT_NE(0u, id1);
  EXPECT_EQ(1u, mgr.database()->groupings().size());

  // Unknown id
  EXPECT_FALSE(mgr.UnregisterService(id1 + 1));

  // Success
  EXPECT_TRUE(mgr.UnregisterService(id1));
  EXPECT_TRUE(mgr.database()->groupings().empty());

  // |id1| becomes unknown
  EXPECT_FALSE(mgr.UnregisterService(id1));
}

TEST(LocalServiceManagerTest, RegisterCharacteristic) {
  LocalServiceManager mgr;

  constexpr IdType kChrcId = 0;
  constexpr uint8_t kChrcProps = Property::kRead;
  constexpr UUID kTestChrcType(uint16_t{0xabcd});
  const att::AccessRequirements kReadReqs(/*encryption=*/true,
                                          /*authentication=*/true,
                                          /*authorization=*/true);
  const att::AccessRequirements kWriteReqs, kUpdateReqs;

  auto service = std::make_unique<Service>(/*primary=*/true, kTestType16);
  service->AddCharacteristic(std::make_unique<Characteristic>(kChrcId,
                                                              kTestChrcType,
                                                              kChrcProps,
                                                              0,
                                                              kReadReqs,
                                                              kWriteReqs,
                                                              kUpdateReqs));
  auto id1 = RegisterService(&mgr, std::move(service));
  EXPECT_NE(0u, id1);

  ASSERT_EQ(1u, mgr.database()->groupings().size());
  const auto& grouping = mgr.database()->groupings().front();
  EXPECT_TRUE(grouping.complete());

  const auto& attrs = grouping.attributes();
  ASSERT_EQ(3u, attrs.size());

  att::Handle srvc_handle = attrs[0].handle();
  EXPECT_EQ(att::kHandleMin, srvc_handle);

  // Characteristic declaration
  EXPECT_EQ(srvc_handle + 1, attrs[1].handle());
  EXPECT_EQ(types::kCharacteristicDeclaration, attrs[1].type());
  EXPECT_EQ(AllowedNoSecurity(), attrs[1].read_reqs());
  EXPECT_EQ(att::AccessRequirements(), attrs[1].write_reqs());
  EXPECT_TRUE(attrs[1].value());

  // clang-format off
  const StaticByteBuffer kDeclValue(
      0x02,        // properties
      0x03, 0x00,  // value handle
      0xcd, 0xab   // UUID
  );
  // clang-format on
  EXPECT_TRUE(ContainersEqual(kDeclValue, *attrs[1].value()));

  // Characteristic value
  EXPECT_EQ(srvc_handle + 2, attrs[2].handle());
  EXPECT_EQ(kTestChrcType, attrs[2].type());
  EXPECT_EQ(kReadReqs, attrs[2].read_reqs());
  EXPECT_EQ(kWriteReqs, attrs[2].write_reqs());

  // This value is dynamic.
  EXPECT_FALSE(attrs[2].value());
}

TEST(LocalServiceManagerTest, RegisterCharacteristic32) {
  LocalServiceManager mgr;

  constexpr IdType kChrcId = 0;
  constexpr uint8_t kChrcProps = Property::kRead;
  constexpr UUID kTestChrcType(uint32_t{0xdeadbeef});
  const att::AccessRequirements kReadReqs(/*encryption=*/true,
                                          /*authentication=*/true,
                                          /*authorization=*/true);
  const att::AccessRequirements kWriteReqs, kUpdateReqs;

  auto service = std::make_unique<Service>(/*primary=*/true, kTestType16);
  service->AddCharacteristic(std::make_unique<Characteristic>(kChrcId,
                                                              kTestChrcType,
                                                              kChrcProps,
                                                              0,
                                                              kReadReqs,
                                                              kWriteReqs,
                                                              kUpdateReqs));
  auto id1 = RegisterService(&mgr, std::move(service));
  EXPECT_NE(0u, id1);

  ASSERT_EQ(1u, mgr.database()->groupings().size());
  const auto& grouping = mgr.database()->groupings().front();
  EXPECT_TRUE(grouping.complete());

  const auto& attrs = grouping.attributes();
  ASSERT_EQ(3u, attrs.size());

  att::Handle srvc_handle = attrs[0].handle();
  EXPECT_EQ(att::kHandleMin, srvc_handle);

  // Characteristic declaration
  EXPECT_EQ(srvc_handle + 1, attrs[1].handle());
  EXPECT_EQ(types::kCharacteristicDeclaration, attrs[1].type());
  EXPECT_EQ(AllowedNoSecurity(), attrs[1].read_reqs());
  EXPECT_EQ(att::AccessRequirements(), attrs[1].write_reqs());
  EXPECT_TRUE(attrs[1].value());

  const StaticByteBuffer kDeclValue(0x02,  // properties
                                    0x03,
                                    0x00,  // value handle

                                    // The 32-bit UUID will be stored as 128-bit
                                    0xFB,
                                    0x34,
                                    0x9B,
                                    0x5F,
                                    0x80,
                                    0x00,
                                    0x00,
                                    0x80,
                                    0x00,
                                    0x10,
                                    0x00,
                                    0x00,
                                    0xef,
                                    0xbe,
                                    0xad,
                                    0xde);
  EXPECT_TRUE(ContainersEqual(kDeclValue, *attrs[1].value()));

  // Characteristic value
  EXPECT_EQ(srvc_handle + 2, attrs[2].handle());
  EXPECT_EQ(kTestChrcType, attrs[2].type());
  EXPECT_EQ(kReadReqs, attrs[2].read_reqs());
  EXPECT_EQ(kWriteReqs, attrs[2].write_reqs());

  // This value is dynamic.
  EXPECT_FALSE(attrs[2].value());
}

TEST(LocalServiceManagerTest, RegisterCharacteristic128) {
  LocalServiceManager mgr;

  constexpr IdType kChrcId = 0;
  constexpr uint8_t kChrcProps = Property::kRead;
  UUID kTestChrcType;
  EXPECT_TRUE(
      StringToUuid("00112233-4455-6677-8899-AABBCCDDEEFF", &kTestChrcType));
  const att::AccessRequirements kReadReqs(/*encryption=*/true,
                                          /*authentication=*/true,
                                          /*authorization=*/true);
  const att::AccessRequirements kWriteReqs, kUpdateReqs;

  auto service = std::make_unique<Service>(/*primary=*/true, kTestType16);
  service->AddCharacteristic(std::make_unique<Characteristic>(kChrcId,
                                                              kTestChrcType,
                                                              kChrcProps,
                                                              0,
                                                              kReadReqs,
                                                              kWriteReqs,
                                                              kUpdateReqs));
  auto id1 = RegisterService(&mgr, std::move(service));
  EXPECT_NE(0u, id1);

  ASSERT_EQ(1u, mgr.database()->groupings().size());
  const auto& grouping = mgr.database()->groupings().front();
  EXPECT_TRUE(grouping.complete());

  const auto& attrs = grouping.attributes();
  ASSERT_EQ(3u, attrs.size());

  att::Handle srvc_handle = attrs[0].handle();
  EXPECT_EQ(att::kHandleMin, srvc_handle);

  // Characteristic declaration
  EXPECT_EQ(srvc_handle + 1, attrs[1].handle());
  EXPECT_EQ(types::kCharacteristicDeclaration, attrs[1].type());
  EXPECT_EQ(AllowedNoSecurity(), attrs[1].read_reqs());
  EXPECT_EQ(att::AccessRequirements(), attrs[1].write_reqs());
  EXPECT_TRUE(attrs[1].value());

  const StaticByteBuffer kDeclValue(0x02,  // properties
                                    0x03,
                                    0x00,  // value handle

                                    // 128-bit UUID
                                    0xFF,
                                    0xEE,
                                    0xDD,
                                    0xCC,
                                    0xBB,
                                    0xAA,
                                    0x99,
                                    0x88,
                                    0x77,
                                    0x66,
                                    0x55,
                                    0x44,
                                    0x33,
                                    0x22,
                                    0x11,
                                    0x00);
  EXPECT_TRUE(ContainersEqual(kDeclValue, *attrs[1].value()));

  // Characteristic value
  EXPECT_EQ(srvc_handle + 2, attrs[2].handle());
  EXPECT_EQ(kTestChrcType, attrs[2].type());
  EXPECT_EQ(kReadReqs, attrs[2].read_reqs());
  EXPECT_EQ(kWriteReqs, attrs[2].write_reqs());

  // This value is dynamic.
  EXPECT_FALSE(attrs[2].value());
}

TEST(LocalServiceManagerTest, ExtPropSetSuccess) {
  LocalServiceManager mgr;
  const att::AccessRequirements kReadReqs, kWriteReqs, kUpdateReqs;
  constexpr UUID kChrcType16(uint16_t{0x1234});
  constexpr IdType kChrcId = 5;

  auto service = std::make_unique<Service>(/*primary=*/true, kTestType16);

  constexpr uint8_t kChrcProps = Property::kRead;
  constexpr uint8_t kExtChrcProps = ExtendedProperty::kReliableWrite;
  auto chrc = std::make_unique<Characteristic>(kChrcId,
                                               kChrcType16,
                                               kChrcProps,
                                               kExtChrcProps,
                                               kReadReqs,
                                               kWriteReqs,
                                               kUpdateReqs);
  service->AddCharacteristic(std::move(chrc));

  ASSERT_TRUE(RegisterService(&mgr, std::move(service)));
  ASSERT_NE(0u, mgr.database()->groupings().size());
  const auto& grouping = mgr.database()->groupings().front();
  const auto& attrs = grouping.attributes();
  ASSERT_EQ(4u, attrs.size());
  EXPECT_EQ(types::kCharacteristicExtProperties, attrs[3].type());
  EXPECT_TRUE(ContainersEqual(  // Reliable Write property
      StaticByteBuffer(0x01, 0x00),
      *attrs[3].value()));
}

// tests that the extended properties descriptor cannot be set externally
TEST(LocalServiceManagerTest, ExtPropSetFailure) {
  LocalServiceManager mgr;
  const att::AccessRequirements kReadReqs, kWriteReqs, kUpdateReqs;

  constexpr UUID kChrcType16(uint16_t{0x1234});
  constexpr UUID kDescType16(uint16_t{0x2900});  // UUID for Ext Prop

  auto service = std::make_unique<Service>(/*primary=*/true, kTestType16);
  auto chrc = std::make_unique<Characteristic>(
      0, kChrcType16, 0, 0, kReadReqs, kWriteReqs, kUpdateReqs);
  chrc->AddDescriptor(
      std::make_unique<Descriptor>(1, kDescType16, kReadReqs, kWriteReqs));
  service->AddCharacteristic(std::move(chrc));

  EXPECT_EQ(false, RegisterService(&mgr, std::move(service)));
}

TEST(LocalServiceManagerTest, RegisterCharacteristicSorted) {
  LocalServiceManager mgr;
  const att::AccessRequirements kReadReqs, kWriteReqs, kUpdateReqs;

  constexpr UUID kType16(uint16_t{0xbeef});
  constexpr UUID kType128(uint32_t{0xdeadbeef});

  constexpr IdType kChrcId0 = 0;
  constexpr uint8_t kChrcProps0 = 0;
  constexpr IdType kChrcId1 = 1;
  constexpr uint8_t kChrcProps1 = 1;
  constexpr IdType kChrcId2 = 2;
  constexpr uint8_t kChrcProps2 = 2;
  constexpr IdType kChrcId3 = 3;
  constexpr uint8_t kChrcProps3 = 3;

  auto service = std::make_unique<Service>(/*primary=*/true, kTestType16);
  service->AddCharacteristic(std::make_unique<Characteristic>(
      kChrcId0, kType128, kChrcProps0, 0, kReadReqs, kWriteReqs, kUpdateReqs));
  service->AddCharacteristic(std::make_unique<Characteristic>(
      kChrcId1, kType16, kChrcProps1, 0, kReadReqs, kWriteReqs, kUpdateReqs));
  service->AddCharacteristic(std::make_unique<Characteristic>(
      kChrcId2, kType128, kChrcProps2, 0, kReadReqs, kWriteReqs, kUpdateReqs));
  service->AddCharacteristic(std::make_unique<Characteristic>(
      kChrcId3, kType16, kChrcProps3, 0, kReadReqs, kWriteReqs, kUpdateReqs));
  auto id1 = RegisterService(&mgr, std::move(service));
  EXPECT_NE(0u, id1);

  ASSERT_EQ(1u, mgr.database()->groupings().size());
  const auto& grouping = mgr.database()->groupings().front();
  EXPECT_TRUE(grouping.complete());

  const auto& attrs = grouping.attributes();
  ASSERT_EQ(9u, attrs.size());

  // The declaration attributes should be sorted by service type (16-bit UUIDs
  // first).
  ASSERT_TRUE(attrs[1].value());
  EXPECT_EQ(kChrcProps1, (*attrs[1].value())[0]);
  ASSERT_TRUE(attrs[3].value());
  EXPECT_EQ(kChrcProps3, (*attrs[3].value())[0]);
  ASSERT_TRUE(attrs[5].value());
  EXPECT_EQ(kChrcProps0, (*attrs[5].value())[0]);
  ASSERT_TRUE(attrs[7].value());
  EXPECT_EQ(kChrcProps2, (*attrs[7].value())[0]);
}

TEST(LocalServiceManagerTest, RegisterDescriptor) {
  LocalServiceManager mgr;
  const att::AccessRequirements kReadReqs, kWriteReqs, kUpdateReqs;

  constexpr UUID kChrcType16(uint16_t{0x1234});
  constexpr UUID kDescType16(uint16_t{0x5678});

  auto service = std::make_unique<Service>(/*primary=*/true, kTestType16);
  auto chrc = std::make_unique<Characteristic>(
      0, kChrcType16, 0, 0, kReadReqs, kWriteReqs, kUpdateReqs);
  chrc->AddDescriptor(
      std::make_unique<Descriptor>(1, kDescType16, kReadReqs, kWriteReqs));
  service->AddCharacteristic(std::move(chrc));

  EXPECT_NE(0u, RegisterService(&mgr, std::move(service)));
  ASSERT_EQ(1u, mgr.database()->groupings().size());
  const auto& grouping = mgr.database()->groupings().front();
  EXPECT_TRUE(grouping.complete());

  const auto& attrs = grouping.attributes();
  ASSERT_EQ(4u, attrs.size());
  EXPECT_EQ(types::kCharacteristicDeclaration, attrs[1].type());
  EXPECT_EQ(kChrcType16, attrs[2].type());
  EXPECT_EQ(kDescType16, attrs[3].type());
  EXPECT_FALSE(attrs[3].value());
}

TEST(LocalServiceManagerTest, DuplicateChrcIds) {
  LocalServiceManager mgr;
  const att::AccessRequirements kReadReqs, kWriteReqs, kUpdateReqs;

  constexpr UUID kChrcType16(uint16_t{0x1234});

  auto service = std::make_unique<Service>(/*primary=*/true, kTestType16);

  // Use same characteristic ID twice.
  service->AddCharacteristic(std::make_unique<Characteristic>(
      0, kChrcType16, 0, 0, kReadReqs, kWriteReqs, kUpdateReqs));
  service->AddCharacteristic(std::make_unique<Characteristic>(
      0, kChrcType16, 0, 0, kReadReqs, kWriteReqs, kUpdateReqs));

  EXPECT_EQ(0u, RegisterService(&mgr, std::move(service)));
}

TEST(LocalServiceManagerTest, DuplicateDescIds) {
  LocalServiceManager mgr;
  const att::AccessRequirements kReadReqs, kWriteReqs, kUpdateReqs;

  constexpr UUID kChrcType16(uint16_t{0x1234});
  constexpr UUID kDescType16(uint16_t{0x5678});

  auto service = std::make_unique<Service>(/*primary=*/true, kTestType16);

  // Use same descriptor ID twice.
  auto chrc = std::make_unique<Characteristic>(
      0, kChrcType16, 0, 0, kReadReqs, kWriteReqs, kUpdateReqs);
  chrc->AddDescriptor(
      std::make_unique<Descriptor>(1, kDescType16, kReadReqs, kWriteReqs));
  chrc->AddDescriptor(
      std::make_unique<Descriptor>(1, kDescType16, kReadReqs, kWriteReqs));
  service->AddCharacteristic(std::move(chrc));

  EXPECT_EQ(0u, RegisterService(&mgr, std::move(service)));
}

TEST(LocalServiceManagerTest, DuplicateChrcAndDescIds) {
  LocalServiceManager mgr;
  const att::AccessRequirements kReadReqs, kWriteReqs, kUpdateReqs;

  constexpr UUID kChrcType16(uint16_t{0x1234});
  constexpr UUID kDescType16(uint16_t{0x5678});

  auto service = std::make_unique<Service>(/*primary=*/true, kTestType16);

  // Use same descriptor ID twice.
  auto chrc = std::make_unique<Characteristic>(
      0, kChrcType16, 0, 0, kReadReqs, kWriteReqs, kUpdateReqs);
  chrc->AddDescriptor(
      std::make_unique<Descriptor>(0, kDescType16, kReadReqs, kWriteReqs));
  service->AddCharacteristic(std::move(chrc));

  EXPECT_EQ(0u, RegisterService(&mgr, std::move(service)));
}

TEST(LocalServiceManagerTest, ReadCharacteristicNoReadPermission) {
  LocalServiceManager mgr;
  const att::AccessRequirements kReadReqs, kWriteReqs, kUpdateReqs;
  constexpr UUID kChrcType16(uint16_t{0x1234});
  constexpr IdType kChrcId = 5;

  auto service = std::make_unique<Service>(/*primary=*/true, kTestType16);
  service->AddCharacteristic(std::make_unique<Characteristic>(kChrcId,
                                                              kChrcType16,
                                                              Property::kRead,
                                                              0,
                                                              kReadReqs,
                                                              kWriteReqs,
                                                              kUpdateReqs));

  bool called = false;
  auto read_cb = [&called](auto, auto, auto, auto, auto) { called = true; };

  EXPECT_NE(0u, RegisterService(&mgr, std::move(service), std::move(read_cb)));

  auto* attr = mgr.database()->FindAttribute(kFirstChrcValueHandle);
  ASSERT_TRUE(attr);
  EXPECT_EQ(kChrcType16, attr->type());

  bool result_called = false;
  auto result_cb = [&result_called](auto, const auto&) {
    result_called = true;
  };

  EXPECT_FALSE(attr->ReadAsync(kTestPeerId, 0, std::move(result_cb)));
  EXPECT_FALSE(called);
  EXPECT_FALSE(result_called);
}

TEST(LocalServiceManagerTest, ReadCharacteristicNoReadProperty) {
  LocalServiceManager mgr;
  constexpr UUID kChrcType16(uint16_t{0x1234});
  constexpr IdType kChrcId = 5;

  // Characteristic is readable but doesn't have the "read" property.
  auto kReadReqs = AllowedNoSecurity();
  const att::AccessRequirements kWriteReqs, kUpdateReqs;

  auto service = std::make_unique<Service>(/*primary=*/true, kTestType16);
  service->AddCharacteristic(std::make_unique<Characteristic>(
      kChrcId, kChrcType16, 0, 0, kReadReqs, kWriteReqs, kUpdateReqs));

  bool called = false;
  auto read_cb = [&called](auto, auto, auto, auto, auto) { called = true; };

  EXPECT_NE(0u, RegisterService(&mgr, std::move(service), std::move(read_cb)));

  auto* attr = mgr.database()->FindAttribute(kFirstChrcValueHandle);
  ASSERT_TRUE(attr);
  EXPECT_EQ(kChrcType16, attr->type());

  fit::result<att::ErrorCode> status = fit::ok();
  auto result_cb = [&status](auto cb_status, const auto&) {
    status = cb_status;
  };

  EXPECT_TRUE(attr->ReadAsync(kTestPeerId, 0, std::move(result_cb)));

  // The error should be handled internally and not reach |read_cb|.
  EXPECT_FALSE(called);
  ASSERT_TRUE(status.is_error());
  EXPECT_EQ(att::ErrorCode::kReadNotPermitted, status.error_value());
}

TEST(LocalServiceManagerTest, ReadCharacteristic) {
  LocalServiceManager mgr;
  constexpr UUID kChrcType16(uint16_t{0x1234});
  constexpr IdType kChrcId = 5;
  constexpr uint16_t kOffset = 10;

  const StaticByteBuffer kTestValue('f', 'o', 'o');

  auto kReadReqs = AllowedNoSecurity();
  const att::AccessRequirements kWriteReqs, kUpdateReqs;

  auto service = std::make_unique<Service>(/*primary=*/true, kTestType16);
  service->AddCharacteristic(std::make_unique<Characteristic>(kChrcId,
                                                              kChrcType16,
                                                              Property::kRead,
                                                              0,
                                                              kReadReqs,
                                                              kWriteReqs,
                                                              kUpdateReqs));

  bool called = false;
  IdType svc_id;
  auto read_cb = [&](PeerId peer_id,
                     auto cb_svc_id,
                     auto id,
                     auto offset,
                     auto responder) {
    called = true;
    EXPECT_EQ(kTestPeerId, peer_id);
    EXPECT_EQ(svc_id, cb_svc_id);
    EXPECT_EQ(kChrcId, id);
    EXPECT_EQ(kOffset, offset);
    responder(fit::ok(), kTestValue);
  };

  svc_id = RegisterService(&mgr, std::move(service), std::move(read_cb));
  ASSERT_NE(0u, svc_id);

  auto* attr = mgr.database()->FindAttribute(kFirstChrcValueHandle);
  ASSERT_TRUE(attr);
  EXPECT_EQ(kChrcType16, attr->type());

  fit::result<att::ErrorCode> status =
      fit::error(att::ErrorCode::kUnlikelyError);
  auto result_cb = [&status, &kTestValue](auto cb_status, const auto& value) {
    status = cb_status;
    EXPECT_TRUE(ContainersEqual(kTestValue, value));
  };

  EXPECT_TRUE(attr->ReadAsync(kTestPeerId, kOffset, std::move(result_cb)));

  EXPECT_TRUE(called);
  EXPECT_EQ(fit::ok(), status);
}

TEST(LocalServiceManagerTest, WriteCharacteristicNoWritePermission) {
  LocalServiceManager mgr;
  const att::AccessRequirements kReadReqs, kWriteReqs, kUpdateReqs;
  constexpr UUID kChrcType16(uint16_t{0x1234});
  constexpr IdType kChrcId = 5;
  const BufferView kTestValue;

  auto service = std::make_unique<Service>(/*primary=*/true, kTestType16);
  service->AddCharacteristic(std::make_unique<Characteristic>(kChrcId,
                                                              kChrcType16,
                                                              Property::kWrite,
                                                              0,
                                                              kReadReqs,
                                                              kWriteReqs,
                                                              kUpdateReqs));

  bool called = false;
  auto write_cb = [&called](auto, auto, auto, auto, auto&, auto) {
    called = true;
  };

  EXPECT_NE(0u,
            RegisterService(
                &mgr, std::move(service), NopReadHandler, std::move(write_cb)));

  auto* attr = mgr.database()->FindAttribute(kFirstChrcValueHandle);
  ASSERT_TRUE(attr);
  EXPECT_EQ(kChrcType16, attr->type());

  bool result_called = false;
  auto result_cb = [&result_called](auto) { result_called = true; };

  EXPECT_FALSE(
      attr->WriteAsync(kTestPeerId, 0, kTestValue, std::move(result_cb)));
  EXPECT_FALSE(called);
  EXPECT_FALSE(result_called);
}

TEST(LocalServiceManagerTest, WriteCharacteristicNoWriteProperty) {
  LocalServiceManager mgr;
  constexpr UUID kChrcType16(uint16_t{0x1234});
  constexpr IdType kChrcId = 5;
  const BufferView kTestValue;

  const att::AccessRequirements kReadReqs, kUpdateReqs;
  auto kWriteReqs = AllowedNoSecurity();

  auto service = std::make_unique<Service>(/*primary=*/true, kTestType16);
  service->AddCharacteristic(std::make_unique<Characteristic>(
      kChrcId, kChrcType16, 0, 0, kReadReqs, kWriteReqs, kUpdateReqs));

  bool called = false;
  auto write_cb = [&called](auto, auto, auto, auto, auto&, auto) {
    called = true;
  };

  EXPECT_NE(0u,
            RegisterService(
                &mgr, std::move(service), NopReadHandler, std::move(write_cb)));

  auto* attr = mgr.database()->FindAttribute(kFirstChrcValueHandle);
  ASSERT_TRUE(attr);
  EXPECT_EQ(kChrcType16, attr->type());

  fit::result<att::ErrorCode> status = fit::ok();
  auto result_cb = [&status](auto cb_status) { status = cb_status; };

  EXPECT_TRUE(
      attr->WriteAsync(kTestPeerId, 0, kTestValue, std::move(result_cb)));

  // The error should be handled internally and not reach |write_cb|.
  EXPECT_FALSE(called);
  ASSERT_TRUE(status.is_error());
  EXPECT_EQ(att::ErrorCode::kWriteNotPermitted, status.error_value());
}

TEST(LocalServiceManagerTest, WriteCharacteristic) {
  LocalServiceManager mgr;
  constexpr UUID kChrcType16(uint16_t{0x1234});
  constexpr IdType kChrcId = 5;
  constexpr uint16_t kOffset = 10;

  const StaticByteBuffer kTestValue('f', 'o', 'o');

  const att::AccessRequirements kReadReqs, kUpdateReqs;
  auto kWriteReqs = AllowedNoSecurity();

  auto service = std::make_unique<Service>(/*primary=*/true, kTestType16);
  service->AddCharacteristic(std::make_unique<Characteristic>(kChrcId,
                                                              kChrcType16,
                                                              Property::kWrite,
                                                              0,
                                                              kReadReqs,
                                                              kWriteReqs,
                                                              kUpdateReqs));

  bool called = false;
  IdType svc_id;
  auto write_cb = [&](PeerId peer_id,
                      auto cb_svc_id,
                      auto id,
                      auto offset,
                      const auto& value,
                      auto responder) {
    called = true;
    EXPECT_EQ(kTestPeerId, peer_id);
    EXPECT_EQ(svc_id, cb_svc_id);
    EXPECT_EQ(kChrcId, id);
    EXPECT_EQ(kOffset, offset);
    EXPECT_TRUE(ContainersEqual(kTestValue, value));
    responder(fit::ok());
  };

  svc_id = RegisterService(
      &mgr, std::move(service), NopReadHandler, std::move(write_cb));
  ASSERT_NE(0u, svc_id);

  auto* attr = mgr.database()->FindAttribute(kFirstChrcValueHandle);
  ASSERT_TRUE(attr);
  EXPECT_EQ(kChrcType16, attr->type());

  fit::result<att::ErrorCode> status =
      fit::error(att::ErrorCode::kUnlikelyError);
  auto result_cb = [&status](auto cb_status) { status = cb_status; };

  EXPECT_TRUE(
      attr->WriteAsync(kTestPeerId, kOffset, kTestValue, std::move(result_cb)));

  EXPECT_TRUE(called);
  EXPECT_EQ(fit::ok(), status);
}

TEST(LocalServiceManagerTest, ReadDescriptorNoReadPermission) {
  LocalServiceManager mgr;
  const att::AccessRequirements kReadReqs, kWriteReqs, kUpdateReqs;
  constexpr UUID kChrcType16(uint16_t{0x1234});
  constexpr UUID kDescType16(uint16_t{0x5678});
  constexpr IdType kChrcId = 0;
  constexpr IdType kDescId = 1;

  auto service = std::make_unique<Service>(/*primary=*/true, kTestType16);
  auto chrc = std::make_unique<Characteristic>(
      kChrcId, kChrcType16, 0, 0, kReadReqs, kWriteReqs, kUpdateReqs);
  chrc->AddDescriptor(std::make_unique<Descriptor>(
      kDescId, kDescType16, kReadReqs, kWriteReqs));
  service->AddCharacteristic(std::move(chrc));

  bool called = false;
  auto read_cb = [&called](auto, auto, auto, auto, auto) { called = true; };

  EXPECT_NE(0u, RegisterService(&mgr, std::move(service), std::move(read_cb)));

  auto* attr = mgr.database()->FindAttribute(kFirstDescrHandle);
  ASSERT_TRUE(attr);
  EXPECT_EQ(kDescType16, attr->type());

  bool result_called = false;
  auto result_cb = [&result_called](auto, const auto&) {
    result_called = true;
  };

  EXPECT_FALSE(attr->ReadAsync(kTestPeerId, 0, std::move(result_cb)));
  EXPECT_FALSE(called);
  EXPECT_FALSE(result_called);
}

TEST(LocalServiceManagerTest, ReadDescriptor) {
  LocalServiceManager mgr;
  constexpr UUID kChrcType16(uint16_t{0x1234});
  constexpr UUID kDescType16(uint16_t{0x5678});
  constexpr IdType kChrcId = 0;
  constexpr IdType kDescId = 1;
  constexpr uint16_t kOffset = 10;

  const StaticByteBuffer kTestValue('f', 'o', 'o');

  auto kReadReqs = AllowedNoSecurity();
  const att::AccessRequirements kWriteReqs, kUpdateReqs;

  auto service = std::make_unique<Service>(/*primary=*/true, kTestType16);
  auto chrc = std::make_unique<Characteristic>(
      kChrcId, kChrcType16, 0, 0, kReadReqs, kWriteReqs, kUpdateReqs);
  chrc->AddDescriptor(
      std::make_unique<Descriptor>(kDescId, kDescType16, kReadReqs, kReadReqs));
  service->AddCharacteristic(std::move(chrc));

  bool called = false;
  IdType svc_id;
  auto read_cb = [&](PeerId peer_id,
                     auto cb_svc_id,
                     auto id,
                     auto offset,
                     auto responder) {
    called = true;
    EXPECT_EQ(kTestPeerId, peer_id);
    EXPECT_EQ(svc_id, cb_svc_id);
    EXPECT_EQ(kDescId, id);
    EXPECT_EQ(kOffset, offset);
    responder(fit::ok(), kTestValue);
  };

  svc_id = RegisterService(&mgr, std::move(service), std::move(read_cb));
  ASSERT_NE(0u, svc_id);

  auto* attr = mgr.database()->FindAttribute(kFirstDescrHandle);
  ASSERT_TRUE(attr);
  EXPECT_EQ(kDescType16, attr->type());

  fit::result<att::ErrorCode> status =
      fit::error(att::ErrorCode::kUnlikelyError);
  auto result_cb = [&status, &kTestValue](auto cb_status, const auto& value) {
    status = cb_status;
    EXPECT_TRUE(ContainersEqual(kTestValue, value));
  };

  EXPECT_TRUE(attr->ReadAsync(kTestPeerId, kOffset, std::move(result_cb)));

  EXPECT_TRUE(called);
  EXPECT_EQ(fit::ok(), status);
}

TEST(LocalServiceManagerTest, WriteDescriptorNoWritePermission) {
  LocalServiceManager mgr;
  const att::AccessRequirements kReadReqs, kWriteReqs, kUpdateReqs;
  constexpr UUID kChrcType16(uint16_t{0x1234});
  constexpr UUID kDescType16(uint16_t{0x5678});
  constexpr IdType kChrcId = 0;
  constexpr IdType kDescId = 1;
  const BufferView kTestValue;

  auto service = std::make_unique<Service>(/*primary=*/true, kTestType16);
  auto chrc = std::make_unique<Characteristic>(
      kChrcId, kChrcType16, 0, 0, kReadReqs, kWriteReqs, kUpdateReqs);
  chrc->AddDescriptor(std::make_unique<Descriptor>(
      kDescId, kDescType16, kReadReqs, kWriteReqs));
  service->AddCharacteristic(std::move(chrc));

  bool called = false;
  auto write_cb = [&called](auto, auto, auto, auto, auto&, auto) {
    called = true;
  };

  EXPECT_NE(
      0u, RegisterService(&mgr, std::move(service), NopReadHandler, write_cb));

  auto* attr = mgr.database()->FindAttribute(kFirstDescrHandle);
  ASSERT_TRUE(attr);
  EXPECT_EQ(kDescType16, attr->type());

  bool result_called = false;
  auto result_cb = [&result_called](auto) { result_called = true; };

  EXPECT_FALSE(attr->WriteAsync(kTestPeerId, 0, kTestValue, result_cb));
  EXPECT_FALSE(called);
  EXPECT_FALSE(result_called);
}

TEST(LocalServiceManagerTest, WriteDescriptor) {
  LocalServiceManager mgr;
  constexpr UUID kChrcType16(uint16_t{0x1234});
  constexpr UUID kDescType16(uint16_t{0x5678});
  constexpr IdType kChrcId = 0;
  constexpr IdType kDescId = 1;
  constexpr uint16_t kOffset = 10;

  const StaticByteBuffer kTestValue('f', 'o', 'o');

  const att::AccessRequirements kReadReqs, kUpdateReqs;
  auto kWriteReqs = AllowedNoSecurity();

  auto service = std::make_unique<Service>(/*primary=*/true, kTestType16);
  auto chrc = std::make_unique<Characteristic>(
      kChrcId, kChrcType16, 0, 0, kReadReqs, kWriteReqs, kUpdateReqs);
  chrc->AddDescriptor(std::make_unique<Descriptor>(
      kDescId, kDescType16, kReadReqs, kWriteReqs));
  service->AddCharacteristic(std::move(chrc));

  bool called = false;
  IdType svc_id;
  auto write_cb = [&](PeerId peer_id,
                      auto cb_svc_id,
                      auto id,
                      auto offset,
                      const auto& value,
                      auto responder) {
    called = true;
    EXPECT_EQ(kTestPeerId, peer_id);
    EXPECT_EQ(svc_id, cb_svc_id);
    EXPECT_EQ(kDescId, id);
    EXPECT_EQ(kOffset, offset);
    EXPECT_TRUE(ContainersEqual(kTestValue, value));
    responder(fit::ok());
  };

  svc_id = RegisterService(&mgr, std::move(service), NopReadHandler, write_cb);
  ASSERT_NE(0u, svc_id);

  auto* attr = mgr.database()->FindAttribute(kFirstDescrHandle);
  ASSERT_TRUE(attr);
  EXPECT_EQ(kDescType16, attr->type());

  fit::result<att::ErrorCode> status =
      fit::error(att::ErrorCode::kUnlikelyError);
  auto result_cb = [&status](auto cb_status) { status = cb_status; };

  EXPECT_TRUE(attr->WriteAsync(kTestPeerId, kOffset, kTestValue, result_cb));

  EXPECT_TRUE(called);
  EXPECT_EQ(fit::ok(), status);
}

TEST(LocalServiceManagerTest, ServiceChanged) {
  IdType expected_id;
  att::Handle expected_start, expected_end;
  int callback_count = 0;
  ServiceChangedCallback service_changed_callback =
      [&](IdType id, att::Handle start, att::Handle end) {
        callback_count++;
        EXPECT_EQ(expected_id, id);
        EXPECT_EQ(expected_start, start);
        EXPECT_EQ(expected_end, end);
      };

  LocalServiceManager mgr;

  // Other tests add and remove services without the callback so there is
  // already the expectation that they work.
  mgr.set_service_changed_callback(std::move(service_changed_callback));

  expected_id = 1u;
  expected_start = 1u;
  expected_end = 1u;
  auto service = std::make_unique<Service>(/*primary=*/true, kTestType16);
  auto id1 = RegisterService(&mgr, std::move(service));
  EXPECT_NE(0u, id1);
  EXPECT_EQ(1, callback_count);

  expected_start = 2u;
  expected_end = 2u;
  service = std::make_unique<Service>(/*primary=*/false, kTestType32);

  constexpr IdType kChrcId = 0;
  constexpr uint8_t kChrcProps = Property::kRead;
  constexpr UUID kTestChrcType(uint32_t{0xdeadbeef});
  const att::AccessRequirements kReadReqs(/*encryption=*/true,
                                          /*authentication=*/true,
                                          /*authorization=*/true);
  const att::AccessRequirements kWriteReqs, kUpdateReqs;
  service->AddCharacteristic(std::make_unique<Characteristic>(kChrcId,
                                                              kTestChrcType,
                                                              kChrcProps,
                                                              0,
                                                              kReadReqs,
                                                              kWriteReqs,
                                                              kUpdateReqs));
  expected_id = 2u;
  expected_start = 2u;
  expected_end = 4u;
  auto id2 = RegisterService(&mgr, std::move(service));
  EXPECT_NE(0u, id2);
  EXPECT_EQ(2, callback_count);

  expected_id = id1;
  expected_start = 1u;
  expected_end = 1u;
  EXPECT_TRUE(mgr.UnregisterService(id1));
  EXPECT_EQ(3, callback_count);

  expected_id = id2;
  expected_start = 2u;
  expected_end = 4u;
  EXPECT_TRUE(mgr.UnregisterService(id2));
  EXPECT_EQ(4, callback_count);
}

// Simulates the attacker's ATT_WRITE_REQ to the Service Changed CCC handle.
// This is the *exact* write handler the production gatt::Server dispatches to
// for an over-the-air CCC write.
void AttackerWriteCCC(LocalServiceManager& mgr, PeerId peer_id) {
  constexpr uint16_t kEnableInd = 0x0002;  // ATT CCC: indications enabled

  // Dynamically locate the Service Changed CCC attribute.
  // (In the default grouping layout where GenericAttributeService is registered
  // first, this is handle 0x0004: 0x0001 service decl, 0x0002 chrc decl,
  // 0x0003 chrc value, 0x0004 CCC descriptor).
  auto iter = mgr.database()->GetIterator(
      att::kHandleMin, att::kHandleMax, &types::kClientCharacteristicConfig);
  const att::Attribute* attr = iter.get();
  ASSERT_TRUE(attr);
  ASSERT_EQ(attr->type(), types::kClientCharacteristicConfig);
  // Confirm the threat-model precondition: write requires no security.
  ASSERT_TRUE(attr->write_reqs().allowed());
  ASSERT_FALSE(attr->write_reqs().encryption_required());
  ASSERT_FALSE(attr->write_reqs().authentication_required());
  ASSERT_FALSE(attr->write_reqs().authorization_required());

  fit::result<att::ErrorCode> status =
      fit::error(att::ErrorCode::kUnlikelyError);
  uint16_t value = pw::bytes::ConvertOrderTo(cpp20::endian::little, kEnableInd);
  bool dispatched =
      attr->WriteAsync(peer_id,
                       /*offset=*/0u,
                       BufferView(&value, sizeof(value)),
                       [&](fit::result<att::ErrorCode> s) { status = s; });
  ASSERT_TRUE(dispatched);
  ASSERT_EQ(status, fit::ok());
}

// Registers a throwaway local service to force OnServiceChanged to walk
// subscribed_peers_.
IdType RegisterTestService(LocalServiceManager& mgr) {
  constexpr UUID kSvcType(uint32_t{0xdeadbeef});
  constexpr UUID kChrcType(uint32_t{0xfeedface});
  const att::AccessRequirements kRead(/*encryption=*/true,
                                      /*authentication=*/true,
                                      /*authorization=*/true);
  const att::AccessRequirements kNone;
  auto service = std::make_unique<Service>(/*primary=*/false, kSvcType);
  service->AddCharacteristic(std::make_unique<Characteristic>(
      /*id=*/0,
      kChrcType,
      Property::kRead,
      /*ext_props=*/0,
      kRead,
      kNone,
      kNone));
  return mgr.RegisterService(
      std::move(service), NopReadHandler, NopWriteHandler, NopCCCallback);
}

// Verify that disconnecting a client removes it from the subscribed peers set,
// preventing memory leaks from repeated connect/disconnect cycles.
TEST(LocalServiceManagerTest, DisconnectShrinksSubscribedPeers) {
  LocalServiceManager mgr;

  size_t indication_calls = 0;
  std::unordered_set<PeerId> indicated_peers;
  auto send_indication = [&](IdType, IdType, PeerId peer_id, BufferView) {
    indication_calls++;
    indicated_peers.insert(peer_id);
  };

  GenericAttributeService gatt_service(mgr.GetWeakPtr(),
                                       std::move(send_indication));
  gatt_service.SetPersistServiceChangedCCCCallback(
      [](PeerId, ServiceChangedCCCPersistedData) {});

  constexpr size_t kCycles = 500;
  for (size_t i = 1; i <= kCycles; ++i) {
    PeerId peer{i};
    AttackerWriteCCC(mgr, peer);
    mgr.DisconnectClient(peer);
  }

  IdType service_id = RegisterTestService(mgr);
  ASSERT_NE(kInvalidId, service_id);

  EXPECT_EQ(0u, indication_calls);
  EXPECT_EQ(0u, indicated_peers.size());

  mgr.UnregisterService(service_id);
}

// TODO(armansito): Some functional groupings of tests above (such as
// ReadCharacteristic, WriteCharacteristic, etc) should each use a common test
// harness to reduce code duplication.
class LocalClientCharacteristicConfigurationTest : public ::testing::Test {
 protected:
  LocalServiceManager mgr;

  // The CCC descriptor is set up as the first descriptor of the first
  // characteristic of the first service:
  //   0x0001: service decl.
  //   0x0002: characteristic decl.
  //   0x0003: characteristic value
  //   0x0004: CCC descriptor
  const att::Handle kChrcHandle = 0x0003;
  const att::Handle kCCCHandle = 0x0004;

  const IdType kChrcId = 0;

  const uint16_t kEnableNot = 0x0001;
  const uint16_t kEnableInd = 0x0002;

  int ccc_callback_count = 0;
  int disconnect_callback_count = 0;
  IdType last_service_id = 0u;
  PeerId last_peer_id = PeerId(kInvalidId);
  PeerId last_disconnected_peer_id = PeerId(kInvalidId);
  bool last_notify = false;
  bool last_indicate = false;

  void BuildService(uint8_t props, const att::AccessRequirements& update_reqs) {
    const att::AccessRequirements kReqs;
    auto service = std::make_unique<Service>(/*is_primary=*/true, kTestType16);
    service->AddCharacteristic(std::make_unique<Characteristic>(
        kChrcId, kTestType32, props, 0, kReqs, kReqs, update_reqs));
    auto ccc_callback = [this](IdType cb_svc_id,
                               IdType id,
                               PeerId peer_id,
                               bool notify,
                               bool indicate) {
      ccc_callback_count++;
      EXPECT_EQ(last_service_id, cb_svc_id);
      EXPECT_EQ(kChrcId, id);
      last_peer_id = peer_id;
      last_notify = notify;
      last_indicate = indicate;
    };
    auto disconnect_callback = [this](IdType cb_svc_id, PeerId peer_id) {
      disconnect_callback_count++;
      EXPECT_EQ(last_service_id, cb_svc_id);
      last_disconnected_peer_id = peer_id;
    };

    const size_t last_service_count = mgr.database()->groupings().size();
    last_service_id = mgr.RegisterService(std::move(service),
                                          NopReadHandler,
                                          NopWriteHandler,
                                          ccc_callback,
                                          std::move(disconnect_callback));
    EXPECT_NE(0u, last_service_id);
    EXPECT_EQ(last_service_count + 1u, mgr.database()->groupings().size());
  }

  bool ReadCcc(const att::Attribute* attr,
               PeerId peer_id,
               fit::result<att::ErrorCode>* out_status,
               uint16_t* out_value) {
    PW_CHECK(attr);
    PW_CHECK(out_status);
    PW_CHECK(out_value);

    auto result_cb = [&out_status, &out_value](auto cb_status,
                                               const auto& value) {
      *out_status = cb_status;
      EXPECT_EQ(2u, value.size());

      if (value.size() == 2u) {
        *out_value = pw::bytes::ConvertOrderFrom(cpp20::endian::little,
                                                 value.template To<uint16_t>());
      }
    };

    return attr->ReadAsync(peer_id, 0u, result_cb);
  }

  bool WriteCcc(const att::Attribute* attr,
                PeerId peer_id,
                uint16_t ccc_value,
                fit::result<att::ErrorCode>* out_status) {
    PW_CHECK(attr);
    PW_CHECK(out_status);

    auto result_cb = [&out_status](auto cb_status) { *out_status = cb_status; };
    uint16_t value =
        pw::bytes::ConvertOrderTo(cpp20::endian::little, ccc_value);
    return attr->WriteAsync(
        peer_id, 0u, BufferView(&value, sizeof(value)), result_cb);
  }
};

TEST_F(LocalClientCharacteristicConfigurationTest, UpdatePermissions) {
  // Require authentication. This should have no bearing on reads but it should
  // prevent writes.
  const att::AccessRequirements kUpdateReqs(/*encryption=*/false,
                                            /*authentication=*/true,
                                            /*authorization=*/false);
  constexpr uint8_t kProps = Property::kNotify;
  BuildService(kProps, kUpdateReqs);

  auto* attr = mgr.database()->FindAttribute(kCCCHandle);
  ASSERT_TRUE(attr);
  EXPECT_EQ(types::kClientCharacteristicConfig, attr->type());

  EXPECT_FALSE(attr->read_reqs().authentication_required());
  EXPECT_TRUE(attr->write_reqs().authentication_required());
}

TEST_F(LocalClientCharacteristicConfigurationTest, IndicationNotSupported) {
  // No security required to enable notifications.
  auto kUpdateReqs = AllowedNoSecurity();
  constexpr uint8_t kProps = Property::kNotify;
  BuildService(kProps, kUpdateReqs);

  auto* attr = mgr.database()->FindAttribute(kCCCHandle);
  ASSERT_TRUE(attr);
  EXPECT_EQ(types::kClientCharacteristicConfig, attr->type());

  // Enabling indications should fail as the characteristic only supports
  // notifications.
  fit::result<att::ErrorCode> status = fit::ok();
  EXPECT_TRUE(WriteCcc(attr, kTestPeerId, kEnableInd, &status));
  ASSERT_TRUE(status.is_error());
  EXPECT_EQ(att::ErrorCode::kWriteNotPermitted, status.error_value());

  uint16_t ccc_value;

  // Notifications and indications for this device should remain disabled.
  EXPECT_TRUE(ReadCcc(attr, kTestPeerId, &status, &ccc_value));
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(0x0000, ccc_value);
}

TEST_F(LocalClientCharacteristicConfigurationTest, NotificationNotSupported) {
  // No security.
  auto kUpdateReqs = AllowedNoSecurity();
  constexpr uint8_t kProps = Property::kIndicate;
  BuildService(kProps, kUpdateReqs);

  auto* attr = mgr.database()->FindAttribute(kCCCHandle);
  ASSERT_TRUE(attr);
  EXPECT_EQ(types::kClientCharacteristicConfig, attr->type());

  // Enabling notifications should fail as the characteristic only supports
  // indications.
  fit::result<att::ErrorCode> status = fit::ok();
  EXPECT_TRUE(WriteCcc(attr, kTestPeerId, kEnableNot, &status));
  ASSERT_TRUE(status.is_error());
  EXPECT_EQ(att::ErrorCode::kWriteNotPermitted, status.error_value());

  uint16_t ccc_value;

  // Notifications and indications for this device should remain disabled.
  EXPECT_TRUE(ReadCcc(attr, kTestPeerId, &status, &ccc_value));
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(0x0000, ccc_value);
}

TEST_F(LocalClientCharacteristicConfigurationTest, EnableNotify) {
  // No security.
  auto kUpdateReqs = AllowedNoSecurity();
  constexpr uint8_t kProps = Property::kNotify;
  BuildService(kProps, kUpdateReqs);

  auto* attr = mgr.database()->FindAttribute(kCCCHandle);
  ASSERT_TRUE(attr);
  EXPECT_EQ(types::kClientCharacteristicConfig, attr->type());

  LocalServiceManager::ClientCharacteristicConfig config;
  EXPECT_FALSE(mgr.GetCharacteristicConfig(
      last_service_id, kChrcId, kTestPeerId, &config));

  fit::result<att::ErrorCode> status =
      fit::error(att::ErrorCode::kUnlikelyError);
  EXPECT_TRUE(WriteCcc(attr, kTestPeerId, kEnableNot, &status));
  EXPECT_EQ(fit::ok(), status);

  uint16_t ccc_value;

  // Notifications should be enabled for kTestPeerId.
  EXPECT_TRUE(ReadCcc(attr, kTestPeerId, &status, &ccc_value));
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(kEnableNot, ccc_value);

  EXPECT_TRUE(mgr.GetCharacteristicConfig(
      last_service_id, kChrcId, kTestPeerId, &config));
  EXPECT_EQ(kChrcHandle, config.handle);
  EXPECT_TRUE(config.notify);
  EXPECT_FALSE(config.indicate);

  // ..but not for kTestPeerId2.
  EXPECT_TRUE(ReadCcc(attr, kTestPeerId2, &status, &ccc_value));
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(0x0000, ccc_value);

  // A set configurations now exists for |kChrcId| but kTestPeerId2 should
  // appear as unsubscribed.
  EXPECT_TRUE(mgr.GetCharacteristicConfig(
      last_service_id, kChrcId, kTestPeerId2, &config));
  EXPECT_EQ(kChrcHandle, config.handle);
  EXPECT_FALSE(config.notify);
  EXPECT_FALSE(config.indicate);

  // The callback should have been notified.
  EXPECT_EQ(1, ccc_callback_count);
  EXPECT_EQ(kTestPeerId, last_peer_id);
  EXPECT_TRUE(last_notify);
  EXPECT_FALSE(last_indicate);

  // Enable notifications again. The write should succeed but the callback
  // should not get called as the value will remain unchanged.
  EXPECT_TRUE(WriteCcc(attr, kTestPeerId, kEnableNot, &status));
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(1, ccc_callback_count);
}

TEST_F(LocalClientCharacteristicConfigurationTest, EnableIndicate) {
  // No security.
  auto kUpdateReqs = AllowedNoSecurity();
  constexpr uint8_t kProps = Property::kIndicate;
  BuildService(kProps, kUpdateReqs);

  auto* attr = mgr.database()->FindAttribute(kCCCHandle);
  ASSERT_TRUE(attr);
  EXPECT_EQ(types::kClientCharacteristicConfig, attr->type());

  fit::result<att::ErrorCode> status =
      fit::error(att::ErrorCode::kUnlikelyError);
  EXPECT_TRUE(WriteCcc(attr, kTestPeerId, kEnableInd, &status));
  EXPECT_EQ(fit::ok(), status);

  uint16_t ccc_value;

  // Indications should be enabled for kTestPeerId.
  EXPECT_TRUE(ReadCcc(attr, kTestPeerId, &status, &ccc_value));
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(kEnableInd, ccc_value);

  LocalServiceManager::ClientCharacteristicConfig config;
  EXPECT_TRUE(mgr.GetCharacteristicConfig(
      last_service_id, kChrcId, kTestPeerId, &config));
  EXPECT_EQ(kChrcHandle, config.handle);
  EXPECT_FALSE(config.notify);
  EXPECT_TRUE(config.indicate);

  // ..but not for kTestPeerId2.
  EXPECT_TRUE(ReadCcc(attr, kTestPeerId2, &status, &ccc_value));
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(0x0000, ccc_value);

  // A set configurations now exists for |kChrcId| but kTestPeerId2 should
  // appear as unsubscribed.
  EXPECT_TRUE(mgr.GetCharacteristicConfig(
      last_service_id, kChrcId, kTestPeerId2, &config));
  EXPECT_EQ(kChrcHandle, config.handle);
  EXPECT_FALSE(config.notify);
  EXPECT_FALSE(config.indicate);

  // The callback should have been notified.
  EXPECT_EQ(1, ccc_callback_count);
  EXPECT_EQ(kTestPeerId, last_peer_id);
  EXPECT_FALSE(last_notify);
  EXPECT_TRUE(last_indicate);

  // Enable indications again. The write should succeed but the callback
  // should not get called as the value will remain unchanged.
  EXPECT_TRUE(WriteCcc(attr, kTestPeerId, kEnableInd, &status));
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(1, ccc_callback_count);
}

// Tests that disconnecting a client cleans up its CCC state and notifies the
// CCC callback if notifications or indications were enabled.
TEST_F(LocalClientCharacteristicConfigurationTest, DisconnectCleanup) {
  auto kUpdateReqs = AllowedNoSecurity();
  constexpr uint8_t kProps = Property::kNotify;
  BuildService(kProps, kUpdateReqs);

  auto* attr = mgr.database()->FindAttribute(kCCCHandle);
  ASSERT_TRUE(attr);
  EXPECT_EQ(types::kClientCharacteristicConfig, attr->type());

  LocalServiceManager::ClientCharacteristicConfig config;
  EXPECT_FALSE(mgr.GetCharacteristicConfig(
      last_service_id, kChrcId, kTestPeerId, &config));

  fit::result<att::ErrorCode> status =
      fit::error(att::ErrorCode::kUnlikelyError);
  EXPECT_TRUE(WriteCcc(attr, kTestPeerId, kEnableNot, &status));
  EXPECT_EQ(fit::ok(), status);

  EXPECT_EQ(1, ccc_callback_count);
  EXPECT_EQ(kTestPeerId, last_peer_id);
  EXPECT_TRUE(last_notify);
  EXPECT_FALSE(last_indicate);

  mgr.DisconnectClient(kTestPeerId);

  uint16_t ccc_value;
  // Reads should succeed but notifications should be disabled.
  EXPECT_TRUE(ReadCcc(attr, kTestPeerId, &status, &ccc_value));
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(0x0000, ccc_value);

  // ccc_callback is not called on client disconnect, only disconnect_callback.
  EXPECT_EQ(1, ccc_callback_count);
  EXPECT_EQ(1, disconnect_callback_count);
  EXPECT_EQ(kTestPeerId, last_disconnected_peer_id);

  mgr.DisconnectClient(kTestPeerId2);

  // disconnect_callback should be called when any client disconnects.
  EXPECT_EQ(1, ccc_callback_count);
  EXPECT_EQ(2, disconnect_callback_count);
  EXPECT_EQ(kTestPeerId2, last_disconnected_peer_id);
}
TEST_F(LocalClientCharacteristicConfigurationTest, WriteCccWithoutResponse) {
  auto kUpdateReqs = AllowedNoSecurity();
  constexpr uint8_t kProps = Property::kNotify;
  BuildService(kProps, kUpdateReqs);

  auto* attr = mgr.database()->FindAttribute(kCCCHandle);
  ASSERT_TRUE(attr);
  EXPECT_EQ(types::kClientCharacteristicConfig, attr->type());

  // Write to CCC with a null callback (simulating Write Command).
  uint16_t value = pw::bytes::ConvertOrderTo(cpp20::endian::little, kEnableNot);

  // This should not crash!
  EXPECT_TRUE(attr->WriteAsync(
      kTestPeerId, 0u, BufferView(&value, sizeof(value)), nullptr));

  uint16_t ccc_value;
  fit::result<att::ErrorCode> status = fit::ok();
  EXPECT_TRUE(ReadCcc(attr, kTestPeerId, &status, &ccc_value));
  EXPECT_EQ(fit::ok(), status);

  // Since we added the check to return early if !result_cb, the value should
  // remain unchanged.
  EXPECT_EQ(0x0000, ccc_value);
  EXPECT_EQ(0, ccc_callback_count);
}
}  // namespace
}  // namespace bt::gatt
